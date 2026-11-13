// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'chrome://resources/cr_elements/cr_button/cr_button.js';
import '/strings.m.js';

import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {CrLitElement} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import type {AgentEvent, BrowserProxy, CaptureStatus, Exchange} from './archive.mojom-webui.js';
import {AgentEventKind, browserProxyFactory, CaptureState, MissReason} from './archive.mojom-webui.js';

// The most rows drawn at once. The panel keeps every exchange the browser
// sends, and draws the latest of those that pass the filter.
const MAX_ROWS = 2000;

export type PanelState = 'none'|'starting'|'archiving'|'stopped'|'replaying';

export type PanelView = 'requests'|'assistant';

// One entry in the conversation with the assistant.
export interface TranscriptItem {
  kind: 'user'|'assistant'|'tool'|'error';
  text: string;
}

export class ArchiveAppElement extends CrLitElement {
  static get is() {
    return 'archive-app';
  }

  static override get styles() {
    return getCss();
  }

  override render() {
    return getHtml.bind(this)();
  }

  static override get properties() {
    return {
      state_: {type: String, reflect: true},
      directory_: {type: String},
      exchangeCount_: {type: Number},
      bodyBytes_: {type: Number},
      missingCount_: {type: Number},
      momentMs_: {type: Number},
      exchanges_: {type: Array},
      filter_: {type: String},
      view_: {type: String},
      transcript_: {type: Array},
      agentBusy_: {type: Boolean},
      question_: {type: String},
    };
  }

  protected accessor state_: PanelState = 'none';
  protected accessor directory_: string = '';
  protected accessor exchangeCount_: number = 0;
  protected accessor bodyBytes_: number = 0;
  protected accessor missingCount_: number = 0;
  protected accessor momentMs_: number = 0;
  protected accessor exchanges_: Exchange[] = [];
  protected accessor filter_: string = '';
  protected accessor view_: PanelView = 'requests';
  protected accessor transcript_: TranscriptItem[] = [];
  protected accessor agentBusy_: boolean = false;
  protected accessor question_: string = '';

  private proxy_: BrowserProxy = browserProxyFactory.getInstance();
  private listenerIds_: number[] = [];

  // The capture the list belongs to. A new one starts a new list.
  private captureNumber_: number = 0;
  // The highest exchange index held, so one sent twice is kept once.
  private lastIndex_: number = 0;
  // Exchanges that arrived while the list was being fetched, to add after.
  private pending_: Exchange[]|null = null;

  override connectedCallback() {
    super.connectedCallback();
    const router = this.proxy_.callbackRouter;
    this.listenerIds_ = [
      router.onCaptureChanged.addListener(
          (status: CaptureStatus) => this.onCaptureChanged_(status)),
      router.onExchangeRecorded.addListener(
          (exchange: Exchange) => this.onExchangeRecorded_(exchange)),
      router.onAgentEvent.addListener(
          (event: AgentEvent) => this.onAgentEvent_(event)),
    ];
    this.fetchCapture_();
    this.proxy_.handler.showUI();
  }

  override disconnectedCallback() {
    super.disconnectedCallback();
    for (const id of this.listenerIds_) {
      this.proxy_.callbackRouter.removeListener(id);
    }
    this.listenerIds_ = [];
  }

  private async fetchCapture_() {
    this.pending_ = [];
    const {status, exchanges} = await this.proxy_.handler.getCapture();
    const pending = this.pending_;
    this.pending_ = null;
    this.applyStatus_(status);
    if (status.state === CaptureState.kNotArchiving) {
      return;
    }
    this.captureNumber_ = status.captureNumber;
    this.exchanges_ = exchanges;
    this.lastIndex_ = exchanges.length ?
        exchanges[exchanges.length - 1]!.index : 0;
    for (const exchange of pending) {
      this.onExchangeRecorded_(exchange);
    }
  }

  private onCaptureChanged_(status: CaptureStatus) {
    if (status.state !== CaptureState.kNotArchiving &&
        status.captureNumber !== this.captureNumber_) {
      // A capture this list is not of: start over with what it holds.
      this.fetchCapture_();
      return;
    }
    this.applyStatus_(status);
  }

  private applyStatus_(status: CaptureStatus) {
    if (status.state === CaptureState.kNotArchiving) {
      // A capture that stopped leaves its list, directory and totals to show.
      this.state_ = this.captureNumber_ ? 'stopped' : 'none';
      return;
    }
    switch (status.state) {
      case CaptureState.kReplaying:
        this.state_ = 'replaying';
        break;
      case CaptureState.kArchiving:
        this.state_ = 'archiving';
        break;
      default:
        this.state_ = 'starting';
    }
    if (status.directory) {
      this.directory_ = status.directory;
    }
    this.exchangeCount_ = status.exchangeCount;
    this.bodyBytes_ = status.bodyBytes;
    this.missingCount_ = status.missingCount;
    this.momentMs_ = status.momentMs;
  }

  private onExchangeRecorded_(exchange: Exchange) {
    if (this.pending_) {
      this.pending_.push(exchange);
      return;
    }
    if (exchange.index <= this.lastIndex_) {
      return;
    }
    this.lastIndex_ = exchange.index;
    this.exchangeCount_ = Math.max(this.exchangeCount_, exchange.index);
    this.bodyBytes_ += exchange.bodyBytes;
    if (exchange.missing) {
      ++this.missingCount_;
    }
    const table = this.shadowRoot?.querySelector<HTMLElement>('#table');
    const atBottom = !table ||
        table.scrollTop + table.clientHeight >= table.scrollHeight - 4;
    this.exchanges_ = [...this.exchanges_, exchange];
    if (atBottom) {
      this.updateComplete.then(() => {
        if (table) {
          table.scrollTop = table.scrollHeight;
        }
      });
    }
  }

  protected onStartClick_() {
    this.proxy_.handler.startArchiving();
  }

  protected onStopClick_() {
    this.proxy_.handler.stopArchiving();
  }

  protected onShowFolderClick_() {
    this.proxy_.handler.showDirectory();
  }

  protected onShowRequestsClick_() {
    this.view_ = 'requests';
  }

  protected onShowAssistantClick_() {
    this.view_ = 'assistant';
    this.updateComplete.then(() => {
      this.shadowRoot?.querySelector<HTMLElement>('#question')?.focus();
    });
  }

  protected onQuestionInput_(e: Event) {
    this.question_ = (e.target as HTMLTextAreaElement).value;
  }

  protected onQuestionKeydown_(e: KeyboardEvent) {
    if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) {
      e.preventDefault();
      this.onAskClick_();
    }
  }

  protected onAskClick_() {
    const question = this.question_.trim();
    if (!question || this.agentBusy_) {
      return;
    }
    this.question_ = '';
    this.agentBusy_ = true;
    this.addToTranscript_({kind: 'user', text: question});
    this.proxy_.handler.askAgent(question);
  }

  protected onStopAgentClick_() {
    this.proxy_.handler.stopAgent();
  }

  private onAgentEvent_(event: AgentEvent) {
    switch (event.kind) {
      case AgentEventKind.kText: {
        const last = this.transcript_[this.transcript_.length - 1];
        if (last && last.kind === 'assistant') {
          // The answer streams in; it is one entry however many pieces.
          this.transcript_ = [
            ...this.transcript_.slice(0, -1),
            {kind: 'assistant', text: last.text + event.text},
          ];
          this.scrollTranscript_();
        } else {
          this.addToTranscript_({kind: 'assistant', text: event.text});
        }
        break;
      }
      case AgentEventKind.kToolCall:
        this.addToTranscript_({kind: 'tool', text: event.text});
        break;
      case AgentEventKind.kIdle:
        this.agentBusy_ = false;
        break;
      case AgentEventKind.kError:
        this.addToTranscript_({kind: 'error', text: event.text});
        break;
    }
  }

  private addToTranscript_(item: TranscriptItem) {
    this.transcript_ = [...this.transcript_, item];
    this.scrollTranscript_();
  }

  // Keeps the latest of the conversation in view, unless the user has
  // scrolled back from it.
  private scrollTranscript_() {
    const transcript =
        this.shadowRoot?.querySelector<HTMLElement>('#transcript');
    if (transcript &&
        transcript.scrollTop + transcript.clientHeight <
            transcript.scrollHeight - 24) {
      return;
    }
    this.updateComplete.then(() => {
      const scroller =
          this.shadowRoot?.querySelector<HTMLElement>('#transcript');
      if (scroller) {
        scroller.scrollTop = scroller.scrollHeight;
      }
    });
  }

  protected onFilterInput_(e: Event) {
    this.filter_ = (e.target as HTMLInputElement).value;
  }

  protected stateText_(): string {
    switch (this.state_) {
      case 'starting':
        return loadTimeData.getString('starting');
      case 'archiving':
        return loadTimeData.getString('archiving');
      case 'stopped':
        return loadTimeData.getString('stopped');
      case 'replaying':
        return this.momentMs_ ?
            loadTimeData.getStringF(
                'replayingAsOf', new Date(this.momentMs_).toLocaleString()) :
            loadTimeData.getString('replaying');
      default:
        return loadTimeData.getString('notArchiving');
    }
  }

  protected isCapturing_(): boolean {
    return this.state_ === 'starting' || this.state_ === 'archiving';
  }

  protected isReplaying_(): boolean {
    return this.state_ === 'replaying';
  }

  protected filterPlaceholder_(): string {
    return loadTimeData.getString(
        this.isReplaying_() ? 'replayFilter' : 'filter');
  }

  protected totalsText_(): string {
    if (this.isReplaying_()) {
      return loadTimeData.getStringF(
          'replayTotals', this.exchangeCount_.toLocaleString(),
          this.missingCount_.toLocaleString(), formatBytes(this.bodyBytes_));
    }
    return loadTimeData.getStringF(
        'totals', this.exchangeCount_.toLocaleString(),
        formatBytes(this.bodyBytes_));
  }

  // The exchanges to draw: those passing the filter, the latest of them.
  protected visibleExchanges_(): Exchange[] {
    const terms = this.filter_.trim().toLowerCase().split(/\s+/)
                      .filter(term => term.length > 0);
    const matching = terms.length === 0 ?
        this.exchanges_ :
        this.exchanges_.filter(exchange => {
          const text = `${exchange.method} ${exchange.statusCode} ${
                           exchange.mimeType} ${exchange.destination} ${
                           exchange.missing ? 'missing' : ''} ${
                           this.missFilterWords_(exchange)} ${
                           exchange.error} ${exchange.capturedUrl ? 'closest' : ''} ${
                           exchange.url}`.toLowerCase();
          return terms.every(term => text.includes(term));
        });
    return matching.slice(-MAX_ROWS);
  }

  // Exchanges recorded before the oldest the panel holds.
  protected olderNotShown_(): number {
    const first = this.exchanges_[0];
    return first ? first.index - 1 : 0;
  }

  protected statusText_(exchange: Exchange): string {
    if (exchange.missing) {
      return loadTimeData.getString('missingStatus');
    }
    if (exchange.error) {
      return '✕';
    }
    return exchange.statusCode ? String(exchange.statusCode) : '—';
  }

  protected statusTitle_(exchange: Exchange): string {
    if (exchange.missing) {
      return loadTimeData.getString('missing');
    }
    if (exchange.error) {
      return loadTimeData.getStringF('failed', exchange.error);
    }
    return exchange.statusCode ? '' : loadTimeData.getString('requestOnly');
  }

  // The media subtype; for a request nothing answered, what it was wanted for.
  protected typeText_(exchange: Exchange): string {
    if (!exchange.mimeType) {
      return exchange.destination || '—';
    }
    const subtype = exchange.mimeType.split('/')[1] || exchange.mimeType;
    return subtype.replace(/^x-/, '').replace(/\+.*$/, '');
  }

  protected sizeText_(exchange: Exchange): string {
    if (exchange.missing) {
      return this.missText_(exchange) || '—';
    }
    if (exchange.error) {
      return '—';
    }
    return (exchange.truncated ? '⚠ ' : '') + formatBytes(exchange.bodyBytes);
  }

  protected answeredByTitle_(exchange: Exchange): string {
    return loadTimeData.getStringF('answeredBy', exchange.capturedUrl);
  }

  protected sizeTitle_(exchange: Exchange): string {
    if (exchange.missing) {
      return this.missTitle_(exchange);
    }
    return exchange.truncated ? loadTimeData.getString('truncated') : '';
  }

  // Why a request the archive held nothing for missed, in a word or two;
  // empty when that is not known.
  protected missText_(exchange: Exchange): string {
    switch (exchange.missReason) {
      case MissReason.kHostNotCaptured:
        return loadTimeData.getString('missHostShort');
      case MissReason.kPathNotCaptured:
        return loadTimeData.getString('missPathShort');
      case MissReason.kOtherQuery:
        return loadTimeData.getString('missQueryShort');
      case MissReason.kOtherMethod:
        return loadTimeData.getString('missMethodShort');
      case MissReason.kRangeNotHeld:
        return loadTimeData.getString('missRangeShort');
      case MissReason.kUnreadable:
        return loadTimeData.getString('missUnreadableShort');
      default:
        return '';
    }
  }

  // Why it missed, in full, and when the capture that came nearest was made.
  protected missTitle_(exchange: Exchange): string {
    let why = '';
    switch (exchange.missReason) {
      case MissReason.kHostNotCaptured:
        why = loadTimeData.getString('missHost');
        break;
      case MissReason.kPathNotCaptured:
        why = loadTimeData.getString('missPath');
        break;
      case MissReason.kOtherQuery:
        why = loadTimeData.getStringF('missQuery', exchange.nearestUrl);
        break;
      case MissReason.kOtherMethod:
        why = loadTimeData.getStringF(
            'missMethod', exchange.nearestMethod, exchange.nearestUrl);
        break;
      case MissReason.kRangeNotHeld:
        why = loadTimeData.getString('missRange');
        break;
      case MissReason.kUnreadable:
        why = loadTimeData.getString(
            exchange.nearestUrl ? 'missUnreadable' : 'archiveUnreadable');
        break;
      default:
        return loadTimeData.getString('missing');
    }
    if (exchange.nearestCapturedMs) {
      why += '\n' +
          loadTimeData.getStringF(
              'capturedAt',
              new Date(exchange.nearestCapturedMs).toLocaleString());
    }
    return why;
  }

  // What the filter finds a miss by: its label, and "never" for both kinds
  // of never captured.
  private missFilterWords_(exchange: Exchange): string {
    if (!exchange.missing) {
      return '';
    }
    const never = exchange.missReason === MissReason.kHostNotCaptured ||
        exchange.missReason === MissReason.kPathNotCaptured;
    return `${this.missText_(exchange)} ${
        never ? loadTimeData.getString('missPathShort') : ''}`;
  }

  // The URL without its scheme, which says little and takes room.
  protected urlText_(exchange: Exchange): string {
    return exchange.url.replace(/^https?:\/\//, '');
  }

  protected rowTitle_(exchange: Exchange): string {
    const lines = [
      `${exchange.method} ${exchange.url}`,
      new Date(exchange.timeMs).toLocaleTimeString(),
    ];
    if (exchange.missing) {
      lines.push(this.missTitle_(exchange));
    }
    if (exchange.capturedUrl) {
      lines.push(this.answeredByTitle_(exchange));
    }
    if (exchange.capturedMs) {
      lines.push(loadTimeData.getStringF(
          'capturedAt', new Date(exchange.capturedMs).toLocaleString()));
    }
    return lines.join('\n');
  }

  protected isError_(exchange: Exchange): boolean {
    return exchange.missing || !!exchange.error || exchange.statusCode === 0 ||
        exchange.statusCode >= 400;
  }
}

function formatBytes(bytes: number): string {
  if (bytes < 1024) {
    return `${bytes} B`;
  }
  const units = ['KB', 'MB', 'GB'];
  let value = bytes / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    ++unit;
  }
  return `${value < 10 ? value.toFixed(1) : Math.round(value)} ${units[unit]}`;
}

declare global {
  interface HTMLElementTagNameMap {
    'archive-app': ArchiveAppElement;
  }
}

customElements.define(ArchiveAppElement.is, ArchiveAppElement);
