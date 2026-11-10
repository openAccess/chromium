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
  <div id="views" role="tablist">
    <button class="view" role="tab" id="requestsView"
        aria-selected="${this.view_ === 'requests'}"
        @click="${this.onShowRequestsClick_}">
      $i18n{requestsView}
    </button>
    <button class="view" role="tab" id="assistantView"
        aria-selected="${this.view_ === 'assistant'}"
        @click="${this.onShowAssistantClick_}">
      $i18n{assistantView}
    </button>
  </div>
</div>
${this.view_ === 'requests' ? html`
<input id="filter" type="search" placeholder="${this.filterPlaceholder_()}"
    .value="${this.filter_}" @input="${this.onFilterInput_}">
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
</div>` : html`
<div id="transcript">
  ${this.transcript_.length === 0 ? html`
    <div class="hint">$i18n{assistantHint}</div>` : ''}
  ${this.transcript_.map(item => html`
    <div class="turn ${item.kind}">${item.text}</div>`)}
  ${this.agentBusy_ ? html`<div class="turn working">…</div>` : ''}
</div>
<div id="ask">
  <textarea id="question" rows="2" placeholder="$i18n{assistantPlaceholder}"
      .value="${this.question_}" @input="${this.onQuestionInput_}"
      @keydown="${this.onQuestionKeydown_}"></textarea>
  ${this.agentBusy_ ? html`
    <cr-button @click="${this.onStopAgentClick_}">$i18n{stopAgent}</cr-button>
  ` : html`
    <cr-button class="action-button" ?disabled="${!this.question_.trim()}"
        @click="${this.onAskClick_}">
      $i18n{ask}
    </cr-button>`}
</div>`}
<!--_html_template_end_-->`;
  // clang-format on
}
