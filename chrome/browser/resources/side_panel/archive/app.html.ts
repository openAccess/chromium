// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {html} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import type {ArchiveAppElement} from './app.js';

export function getHtml(this: ArchiveAppElement) {
  // clang-format off
  return html`<!--_html_template_start_-->
<div id="summary">
  <div id="state"><span id="dot"></span>${this.stateText_()}</div>
  ${this.directory_ ? html`
    <div id="directory" title="${this.directory_}">${this.directory_}</div>
    <div id="totals">${this.totalsText_()}</div>` : html`
    <div class="hint">$i18n{notArchivingHint}</div>`}
  <div id="buttons">
    ${this.isReplaying_() ? html`
      <cr-button ?disabled="${!this.directory_}"
          @click="${this.onShowFolderClick_}">
        $i18n{showArchive}
      </cr-button>` : html`
    ${this.isCapturing_() ? html`
      <cr-button class="action-button" @click="${this.onStopClick_}">
        $i18n{stopArchiving}
      </cr-button>` : html`
      <cr-button class="action-button" @click="${this.onStartClick_}">
        $i18n{startArchiving}
      </cr-button>`}
    <cr-button ?disabled="${!this.directory_}"
        @click="${this.onShowFolderClick_}">
      $i18n{showFolder}
    </cr-button>`}
  </div>
  <input id="filter" type="search" placeholder="${this.filterPlaceholder_()}"
      .value="${this.filter_}" @input="${this.onFilterInput_}">
</div>
<div id="table">
  <div class="row head">
    <span>$i18n{statusColumn}</span><span>$i18n{typeColumn}</span>
    <span class="size">$i18n{sizeColumn}</span><span>$i18n{urlColumn}</span>
  </div>
  ${this.olderNotShown_() > 0 ? html`
    <div class="hint">
      ${loadTimeData.getStringF('olderNotShown',
                                this.olderNotShown_().toLocaleString())}
    </div>` : ''}
  ${this.visibleExchanges_().map(exchange => html`
    <div class="row" title="${this.rowTitle_(exchange)}">
      <span class="${this.isError_(exchange) ? 'error' : ''}"
          title="${this.statusTitle_(exchange)}">
        ${this.statusText_(exchange)}
      </span>
      <span>${this.typeText_(exchange)}</span>
      <span class="size" title="${this.sizeTitle_(exchange)}">
        ${this.sizeText_(exchange)}
      </span>
      <span>${exchange.capturedUrl ? html`
        <span class="closest" title="${this.answeredByTitle_(exchange)}">≈</span>`
          : ''}${exchange.method !== 'GET' ? html`
        <span class="method">${exchange.method}</span>` : ''}${
          this.urlText_(exchange)}</span>
    </div>`)}
</div>
<!--_html_template_end_-->`;
  // clang-format on
}
