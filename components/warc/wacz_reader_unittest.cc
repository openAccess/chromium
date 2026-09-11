// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/warc/wacz_reader.h"

#include <string>
#include <string_view>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/zlib/google/zip.h"

namespace warc {
namespace {

std::string ToString(const std::vector<uint8_t>& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

class WaczReaderTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  // Puts `contents` at `name` inside the container being built.
  void AddFile(std::string_view name, std::string_view contents) {
    const base::FilePath path = ContentDir().AppendASCII(name);
    ASSERT_TRUE(base::CreateDirectory(path.DirName()));
    ASSERT_TRUE(base::WriteFile(path, contents));
  }

  // Zips what has been added. zip::Zip stores ".gz" entries and deflates the
  // rest, which is the split a WACZ requires, so the container built here has
  // the shape of a real one.
  base::FilePath BuildContainer() {
    const base::FilePath path = temp_dir_.GetPath().AppendASCII("out.wacz");
    EXPECT_TRUE(zip::Zip(ContentDir(), path, /*include_hidden_files=*/false));
    return path;
  }

  std::optional<WaczReader> OpenContainer() {
    return WaczReader::Open(base::File(
        BuildContainer(), base::File::FLAG_OPEN | base::File::FLAG_READ));
  }

  base::FilePath ContentDir() {
    return temp_dir_.GetPath().AppendASCII("content");
  }

  base::ScopedTempDir temp_dir_;
};

TEST_F(WaczReaderTest, ReadsTheEntriesOfAContainer) {
  AddFile("datapackage.json", R"({"profile": "data-package"})");
  AddFile("pages/pages.jsonl", "{\"url\": \"http://example.org/\"}\n");
  AddFile("archive/data.warc.gz", "pretend this is a gzipped archive");

  std::optional<WaczReader> reader = OpenContainer();
  ASSERT_TRUE(reader.has_value());

  const WaczReader::Entry* archive = reader->Find("archive/data.warc.gz");
  ASSERT_TRUE(archive);
  EXPECT_EQ(33u, archive->uncompressed_size);
  // Stored, which is what makes seeking into it possible at all.
  EXPECT_TRUE(archive->stored);
  EXPECT_GT(archive->data_offset, 0u);

  const WaczReader::Entry* package = reader->Find("datapackage.json");
  ASSERT_TRUE(package);
  EXPECT_FALSE(package->stored);

  EXPECT_FALSE(reader->Find("nothing/here"));
}

TEST_F(WaczReaderTest, ReadsAStoredEntryAtAnOffset) {
  // The property the whole reader exists for: an archive is read where it
  // lies, at the offset an index gave, without unpacking the rest of it.
  std::string archive(4096, '.');
  const std::string record = "WARC/1.1 a record sitting at a known offset";
  archive.replace(1000, record.size(), record);
  AddFile("archive/data.warc.gz", archive);

  std::optional<WaczReader> reader = OpenContainer();
  ASSERT_TRUE(reader.has_value());
  const WaczReader::Entry* entry = reader->Find("archive/data.warc.gz");
  ASSERT_TRUE(entry);

  std::optional<std::vector<uint8_t>> got =
      reader->ReadAt(*entry, 1000, record.size());
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(record, ToString(*got));

  // A read that runs past the entry would return whatever the container holds
  // next, which is not part of this archive.
  EXPECT_FALSE(reader->ReadAt(*entry, 4090, 100).has_value());
  EXPECT_FALSE(reader->ReadAt(*entry, 5000, 1).has_value());
}

TEST_F(WaczReaderTest, ReadsASmallEntryWhole) {
  const std::string package = R"({"profile": "data-package", "resources": []})";
  AddFile("datapackage.json", package);

  std::optional<WaczReader> reader = OpenContainer();
  ASSERT_TRUE(reader.has_value());
  const WaczReader::Entry* entry = reader->Find("datapackage.json");
  ASSERT_TRUE(entry);

  // Deflated in the container, and wanted whole rather than at an offset.
  ASSERT_FALSE(entry->stored);
  std::optional<std::vector<uint8_t>> got = reader->ReadEntry(*entry);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(package, ToString(*got));
}

TEST_F(WaczReaderTest, RefusesAnOffsetIntoADeflatedEntry) {
  AddFile("datapackage.json", std::string(2048, 'x'));

  std::optional<WaczReader> reader = OpenContainer();
  ASSERT_TRUE(reader.has_value());
  const WaczReader::Entry* entry = reader->Find("datapackage.json");
  ASSERT_TRUE(entry);
  ASSERT_FALSE(entry->stored);

  // Position 100 of a deflated entry is a position in the compressed bytes,
  // which corresponds to nothing in particular in what they decode to.
  // Answering with those bytes would be worse than refusing.
  EXPECT_FALSE(reader->ReadAt(*entry, 100, 10).has_value());
}

TEST_F(WaczReaderTest, FindsTheArchivesOfAContainer) {
  AddFile("archive/first.warc.gz", "one");
  AddFile("archive/second.warc.gz", "two");
  AddFile("indexes/index.cdx.gz", "index");
  AddFile("datapackage.json", "{}");

  std::optional<WaczReader> reader = OpenContainer();
  ASSERT_TRUE(reader.has_value());

  // A capture that rotated is several archives in one container, so the
  // reader has to hand back all of them.
  std::vector<const WaczReader::Entry*> archives =
      reader->FindAllWithPrefix("archive/");
  ASSERT_EQ(2u, archives.size());
  std::vector<std::string> names{archives[0]->name, archives[1]->name};
  std::sort(names.begin(), names.end());
  EXPECT_EQ("archive/first.warc.gz", names[0]);
  EXPECT_EQ("archive/second.warc.gz", names[1]);

  EXPECT_EQ(1u, reader->FindAllWithPrefix("indexes/").size());
  EXPECT_EQ(0u, reader->FindAllWithPrefix("nothing/").size());
}

TEST_F(WaczReaderTest, RefusesWhatIsNotAContainer) {
  const base::FilePath path = temp_dir_.GetPath().AppendASCII("not.wacz");
  ASSERT_TRUE(base::WriteFile(path, "this is not a zip file at all"));
  EXPECT_FALSE(WaczReader::Open(
                   base::File(path, base::File::FLAG_OPEN |
                                        base::File::FLAG_READ))
                   .has_value());

  EXPECT_FALSE(WaczReader::Open(base::File()).has_value());
}

TEST_F(WaczReaderTest, RefusesATruncatedContainer) {
  AddFile("archive/data.warc.gz", std::string(4096, 'z'));
  const base::FilePath whole = BuildContainer();
  std::string bytes;
  ASSERT_TRUE(base::ReadFileToString(whole, &bytes));

  // The directory is at the end, so a container cut short has lost the only
  // part that says what is in it.
  const base::FilePath cut = temp_dir_.GetPath().AppendASCII("cut.wacz");
  ASSERT_TRUE(base::WriteFile(cut, std::string_view(bytes).substr(
                                       0, bytes.size() / 2)));
  EXPECT_FALSE(
      WaczReader::Open(
          base::File(cut, base::File::FLAG_OPEN | base::File::FLAG_READ))
          .has_value());
}

}  // namespace
}  // namespace warc
