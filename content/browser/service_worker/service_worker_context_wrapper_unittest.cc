// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_worker/service_worker_context_wrapper.h"

#include <memory>
#include <vector>

#include "base/files/scoped_temp_dir.h"
#include "base/run_loop.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/bind.h"
#include "base/test/scoped_feature_list.h"
#include "base/unguessable_token.h"
#include "components/services/storage/service_worker/service_worker_storage_control_impl.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_context_wrapper_test_api.h"
#include "content/browser/service_worker/service_worker_registration.h"
#include "content/browser/service_worker/service_worker_test_utils.h"
#include "content/browser/service_worker/service_worker_version.h"
#include "content/browser/storage_partition_impl.h"
#include "content/public/browser/storage_partition_config.h"
#include "content/public/common/content_client.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/test_content_browser_client.h"
#include "net/base/features.h"
#include "net/base/schemeful_site.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/client_security_state.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/mojom/service_worker/service_worker_registration.mojom.h"
#include "url/origin.h"

namespace content {

// Fixture for the ServiceWorkerContextWrapper test. It uses a disk user data
// directory in order to test starting the browser with a registration already
// written to storage.
class ServiceWorkerContextWrapperTest : public testing::Test {
 public:
  ServiceWorkerContextWrapperTest() = default;

  ServiceWorkerContextWrapperTest(const ServiceWorkerContextWrapperTest&) =
      delete;
  ServiceWorkerContextWrapperTest& operator=(
      const ServiceWorkerContextWrapperTest&) = delete;

  void SetUp() override {
    ASSERT_TRUE(user_data_directory_.CreateUniqueTempDir());
    InitWrapper();
  }

  void TearDown() override {
    // Shutdown or else ASAN complains of leaks.
    wrapper_->Shutdown();
    base::RunLoop().RunUntilIdle();
  }

  void InitWrapper() {
    browser_context_ = std::make_unique<TestBrowserContext>();
    wrapper_ = base::MakeRefCounted<ServiceWorkerContextWrapper>(
        browser_context_.get());
    // Set up a mojo connection binder which binds a connection to a
    // ServiceWorkerStorageControlImpl instance owned by `this`. This is needed
    // to work around strange situations (e.g. nested
    // ServiceWorkerContextWrapper::Init() calls) caused by overwriting
    // StoragePartitionImpl the below.
    wrapper_->SetStorageControlBinderForTest(base::BindRepeating(
        &ServiceWorkerContextWrapperTest::BindStorageControl,
        base::Unretained(this)));
    StoragePartitionImpl* storage_partition =
        static_cast<StoragePartitionImpl*>(
            browser_context_->GetStoragePartitionForUrl(
                GURL("https://example.com")));
    ServiceWorkerContextWrapperTestApi(wrapper_.get())
        .set_storage_partition(storage_partition);
    wrapper_->Init(user_data_directory_.GetPath(), nullptr, nullptr, nullptr);
    // Init() posts a couple tasks to the IO thread. Let them finish.
    base::RunLoop().RunUntilIdle();
  }

  ServiceWorkerContextCore* context() { return wrapper_->context(); }
  ServiceWorkerRegistry& registry() { return context()->registry(); }

  blink::ServiceWorkerStatusCode StoreRegistration(
      scoped_refptr<ServiceWorkerRegistration> registration) {
    blink::ServiceWorkerStatusCode result;
    base::RunLoop loop;
    registry().StoreRegistration(
        registration.get(), registration->waiting_version(),
        base::BindLambdaForTesting([&](blink::ServiceWorkerStatusCode status) {
          result = status;
          loop.Quit();
        }));
    loop.Run();
    return result;
  }

  blink::ServiceWorkerStatusCode DeleteRegistration(
      scoped_refptr<ServiceWorkerRegistration> registration) {
    blink::ServiceWorkerStatusCode result;
    base::RunLoop loop;
    registry().DeleteRegistration(
        registration,
        base::BindLambdaForTesting([&](blink::ServiceWorkerStatusCode status) {
          result = status;
          loop.Quit();
        }));
    loop.Run();
    return result;
  }

 protected:
  void BindStorageControl(
      mojo::PendingReceiver<storage::mojom::ServiceWorkerStorageControl>
          receiver) {
    storage_control_ =
        std::make_unique<storage::ServiceWorkerStorageControlImpl>(
            user_data_directory_.GetPath(),
            base::MakeRefCounted<
                storage::ServiceWorkerStorage::StorageSharedBuffer>(),
            std::move(receiver));
  }

  BrowserTaskEnvironment task_environment_{BrowserTaskEnvironment::IO_MAINLOOP};
  base::ScopedTempDir user_data_directory_;
  std::unique_ptr<TestBrowserContext> browser_context_;
  scoped_refptr<ServiceWorkerContextWrapper> wrapper_;
  std::unique_ptr<storage::ServiceWorkerStorageControlImpl> storage_control_;
};

// Test that the UI thread knows which origins have registrations upon
// browser startup. Regression test for https://crbug.com/991143.
TEST_F(ServiceWorkerContextWrapperTest, HasRegistration) {
  // Make a service worker.
  GURL scope("https://example.com/");
  const blink::StorageKey key =
      blink::StorageKey::CreateFirstParty(url::Origin::Create(scope));
  GURL script("https://example.com/sw.js");
  scoped_refptr<ServiceWorkerRegistration> registration =
      CreateServiceWorkerRegistrationAndVersion(context(), scope, script, key,
                                                /*resource_id=*/1);

  // Store it.
  base::RunLoop loop;
  registry().StoreRegistration(
      registration.get(), registration->waiting_version(),
      base::BindLambdaForTesting(
          [&loop](blink::ServiceWorkerStatusCode status) {
            ASSERT_EQ(blink::ServiceWorkerStatusCode::kOk, status);
            loop.Quit();
          }));
  loop.Run();

  // Simulate browser shutdown and restart.
  wrapper_->Shutdown();
  base::RunLoop().RunUntilIdle();
  wrapper_.reset();
  InitWrapper();

  // Now test that registrations are recognized.
  wrapper_->context()->registry().WaitForRegistrationsInitializedForTest();
  EXPECT_TRUE(wrapper_->MaybeHasRegistrationForStorageKey(key));
  EXPECT_FALSE(wrapper_->MaybeHasRegistrationForStorageKey(
      blink::StorageKey::CreateFromStringForTesting("https://example.org")));
}

// This test involves storing two registrations for the same key to storage
// and deleting one of them to check that MaybeHasRegistrationForStorageKey
// still correctly returns TRUE since there is still one registration for the
// key, and should only return FALSE when ALL registrations for that key
// have been deleted from storage.
TEST_F(ServiceWorkerContextWrapperTest, DeleteRegistrationsForSameKey) {
  wrapper_->context()->registry().WaitForRegistrationsInitializedForTest();

  // Make two registrations for same origin.
  GURL scope1("https://example1.com/abc/");
  const blink::StorageKey key =
      blink::StorageKey::CreateFirstParty(url::Origin::Create(scope1));
  GURL script1("https://example1.com/abc/sw.js");
  scoped_refptr<ServiceWorkerRegistration> registration1 =
      CreateServiceWorkerRegistrationAndVersion(context(), scope1, script1, key,
                                                /*resource_id=*/1);
  GURL scope2("https://example1.com/xyz/");
  GURL script2("https://example1.com/xyz/sw.js");
  scoped_refptr<ServiceWorkerRegistration> registration2 =
      CreateServiceWorkerRegistrationAndVersion(context(), scope2, script2, key,
                                                1);

  // Store both registrations.
  ASSERT_EQ(StoreRegistration(registration1),
            blink::ServiceWorkerStatusCode::kOk);
  ASSERT_EQ(StoreRegistration(registration2),
            blink::ServiceWorkerStatusCode::kOk);

  // Delete one of the registrations.
  ASSERT_EQ(DeleteRegistration(registration1),
            blink::ServiceWorkerStatusCode::kOk);

  // Run loop until idle to wait for
  // ServiceWorkerRegistry::DidDeleteRegistration() to be executed, and make
  // sure that NotifyAllRegistrationsDeletedForStorageKey() is not called.
  base::RunLoop().RunUntilIdle();

  // Now test that a registration for a key is still recognized.
  EXPECT_TRUE(wrapper_->MaybeHasRegistrationForStorageKey(key));

  // Remove second registration.
  ASSERT_EQ(DeleteRegistration(registration2),
            blink::ServiceWorkerStatusCode::kOk);

  // Run loop until idle to wait for
  // ServiceWorkerRegistry::DidDeleteRegistration() to be executed, and make
  // sure that this time NotifyAllRegistrationsDeletedForStorageKey() is called.
  base::RunLoop().RunUntilIdle();

  // Now test that key does not have any registrations.
  EXPECT_FALSE(wrapper_->MaybeHasRegistrationForStorageKey(key));
}

// This tests installs two registrations with the same origin but different
// top-level site, then deletes one, then confirms that the other still appears
// for MaybeHasRegistrationForStorageKey().
TEST_F(ServiceWorkerContextWrapperTest, DeleteRegistrationsForPartitionedKeys) {
  base::test::ScopedFeatureList scope_feature_list_;
  scope_feature_list_.InitAndEnableFeature(
      net::features::kThirdPartyStoragePartitioning);

  wrapper_->context()->registry().WaitForRegistrationsInitializedForTest();

  // Make two registrations for same origin, but different top-level site.
  GURL scope("https://example1.com/abc/");
  net::SchemefulSite site1 = net::SchemefulSite(GURL("https://site1.example"));
  blink::StorageKey key1 =
      blink::StorageKey::Create(url::Origin::Create(scope), site1,
                                blink::mojom::AncestorChainBit::kCrossSite);
  GURL script("https://example1.com/abc/sw.js");
  scoped_refptr<ServiceWorkerRegistration> registration1 =
      CreateServiceWorkerRegistrationAndVersion(context(), scope, script, key1,
                                                /*resource_id=*/1);

  net::SchemefulSite site2 = net::SchemefulSite(GURL("https://site2.example"));
  blink::StorageKey key2 =
      blink::StorageKey::Create(url::Origin::Create(scope), site2,
                                blink::mojom::AncestorChainBit::kCrossSite);
  scoped_refptr<ServiceWorkerRegistration> registration2 =
      CreateServiceWorkerRegistrationAndVersion(context(), scope, script, key2,
                                                2);

  // Store both registrations.
  ASSERT_EQ(StoreRegistration(registration1),
            blink::ServiceWorkerStatusCode::kOk);
  ASSERT_EQ(StoreRegistration(registration2),
            blink::ServiceWorkerStatusCode::kOk);

  // Delete one of the registrations.
  ASSERT_EQ(DeleteRegistration(registration1),
            blink::ServiceWorkerStatusCode::kOk);

  // Run loop until idle to wait for
  // ServiceWorkerRegistry::DidDeleteRegistration() to be executed, and make
  // sure that NotifyAllRegistrationsDeletedForStorageKey() is not called.
  base::RunLoop().RunUntilIdle();

  // The first key should be gone.
  EXPECT_FALSE(wrapper_->MaybeHasRegistrationForStorageKey(key1));
  // Now test that a registration for the second is still recognized.
  EXPECT_TRUE(wrapper_->MaybeHasRegistrationForStorageKey(key2));

  // Remove second registration.
  ASSERT_EQ(DeleteRegistration(registration2),
            blink::ServiceWorkerStatusCode::kOk);

  // Run loop until idle to wait for
  // ServiceWorkerRegistry::DidDeleteRegistration() to be executed, and make
  // sure that this time NotifyAllRegistrationsDeletedForStorageKey() is called.
  base::RunLoop().RunUntilIdle();

  // Now test that key does not have any registrations.
  EXPECT_FALSE(wrapper_->MaybeHasRegistrationForStorageKey(key2));
}

// This tests deleting registrations from storage and checking that even if live
// registrations may exist, MaybeHasRegistrationForStorageKey correctly returns
// FALSE since the registrations do not exist in storage.
TEST_F(ServiceWorkerContextWrapperTest, DeleteRegistration) {
  wrapper_->context()->registry().WaitForRegistrationsInitializedForTest();

  // Make registration.
  GURL scope1("https://example2.com/");
  const blink::StorageKey key =
      blink::StorageKey::CreateFirstParty(url::Origin::Create(scope1));
  GURL script1("https://example2.com/");
  scoped_refptr<ServiceWorkerRegistration> registration =
      CreateServiceWorkerRegistrationAndVersion(context(), scope1, script1, key,
                                                /*resource_id=*/1);

  // Store registration.
  ASSERT_EQ(StoreRegistration(registration),
            blink::ServiceWorkerStatusCode::kOk);

  wrapper_->OnRegistrationCompleted(registration->id(), registration->scope(),
                                    registration->key());
  base::RunLoop().RunUntilIdle();

  // Now test that a registration is recognized.
  EXPECT_TRUE(wrapper_->MaybeHasRegistrationForStorageKey(key));

  // Delete registration from storage.
  ASSERT_EQ(DeleteRegistration(registration),
            blink::ServiceWorkerStatusCode::kOk);

  // Finish deleting registration from storage.
  base::RunLoop().RunUntilIdle();

  // Now test that key does not have any registrations. This should return
  // FALSE even when live registrations may exist, as the registrations have
  // been deleted from storage.
  EXPECT_FALSE(wrapper_->MaybeHasRegistrationForStorageKey(key));
}

// Notes which of the two hooks a factory with no frame or process was offered
// to, in order, and takes it at the first if told to.
class PartitionFactoryBrowserClient : public TestContentBrowserClient {
 public:
  explicit PartitionFactoryBrowserClient(bool take) : take_(take) {}

  bool WillCreateURLLoaderFactoryForStoragePartition(
      BrowserContext* browser_context,
      const StoragePartitionConfig& config,
      URLLoaderFactoryType type,
      network::URLLoaderFactoryBuilder& factory_builder) override {
    offers_.push_back("partition");
    partition_ = config;
    type_ = type;
    return take_;
  }

  void WillCreateURLLoaderFactory(
      BrowserContext* browser_context,
      RenderFrameHost* frame,
      int render_process_id,
      URLLoaderFactoryType type,
      const url::Origin& request_initiator,
      const net::IsolationInfo& isolation_info,
      std::optional<int64_t> navigation_id,
      ukm::SourceIdObj ukm_source_id,
      network::URLLoaderFactoryBuilder& factory_builder,
      mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>*
          header_client,
      bool* bypass_redirect_checks,
      bool* disable_secure_dns,
      network::mojom::URLLoaderFactoryOverridePtr* factory_override,
      scoped_refptr<base::SequencedTaskRunner> navigation_response_task_runner,
      bool is_for_network_service) override {
    offers_.push_back("frame or process");
  }

  const std::vector<std::string>& offers() const { return offers_; }
  const std::optional<StoragePartitionConfig>& partition() const {
    return partition_;
  }
  std::optional<URLLoaderFactoryType> type() const { return type_; }

 private:
  const bool take_;
  std::vector<std::string> offers_;
  std::optional<StoragePartitionConfig> partition_;
  std::optional<URLLoaderFactoryType> type_;
};

// A service worker's main script is fetched before the worker has a process,
// and no frame is the worker's, so the partition is all that says whose the
// factory is: the embedder is told it first, and only then asked as for a
// frame or a process. So is a check for a newer script, which is made the same
// way -- and, unlike the first fetch, needs no worker under DevTools to test.
TEST_F(ServiceWorkerContextWrapperTest,
       AMainScriptFactoryIsOfferedByItsPartitionFirst) {
  PartitionFactoryBrowserClient client(/*take=*/false);
  ContentBrowserClient* old_client = SetBrowserClientForTesting(&client);

  EXPECT_TRUE(wrapper_->GetLoaderFactoryForUpdateCheck(
      GURL("https://example.com/"), network::mojom::ClientSecurityState::New(),
      base::UnguessableToken::Create()));
  EXPECT_EQ((std::vector<std::string>{"partition", "frame or process"}),
            client.offers());
  EXPECT_EQ(wrapper_->storage_partition()->GetConfig(), client.partition());
  EXPECT_EQ(ContentBrowserClient::URLLoaderFactoryType::kServiceWorkerScript,
            client.type());

  SetBrowserClientForTesting(old_client);
}

// An embedder that serves the partition from somewhere else takes the factory
// whole, and it is offered to nothing after.
TEST_F(ServiceWorkerContextWrapperTest,
       AMainScriptFactoryTheEmbedderTakesIsOfferedNoFurther) {
  PartitionFactoryBrowserClient client(/*take=*/true);
  ContentBrowserClient* old_client = SetBrowserClientForTesting(&client);

  EXPECT_TRUE(wrapper_->GetLoaderFactoryForUpdateCheck(
      GURL("https://example.com/"), network::mojom::ClientSecurityState::New(),
      base::UnguessableToken::Create()));
  EXPECT_EQ(std::vector<std::string>{"partition"}, client.offers());

  SetBrowserClientForTesting(old_client);
}

}  // namespace content
