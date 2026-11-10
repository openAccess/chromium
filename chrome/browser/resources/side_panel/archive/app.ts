// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'chrome://resources/cr_elements/cr_button/cr_button.js';
import '/strings.m.js';

import {loadTimeData} from 'chrome://resources/js/load_time_data.js';
import {CrLitElement} from 'chrome://resources/lit/v3_0/lit.rollup.js';

import {getCss} from './app.css.js';
import {getHtml} from './app.html.js';
import type {BrowserProxy, CaptureStatus, Exchange} from './archive.mojom-webui.js';
import {browserProxyFactory, CaptureState} from './archive.mojom-webui.js';

// The most rows drawn at once. The panel keeps every exchange the browser
// sends, and draws the latest of those that pass the filter.
const MAX_ROWS = 2000;

export type PanelState = 'none'|'starting'|'archiving'|'stopped';

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
      exchanges_: {type: Array},
      filter_: {type: String},
    };
  }

  protected accessor state_: PanelState = 'none';
  protected accessor directory_: string = '';
  protected accessor exchangeCount_: number = 0;
  protected accessor bodyBytes_: number = 0;
  protected accessor exchanges_: Exchange[] = [];
  protected accessor filter_: string = '';

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
    this.state_ =
        status.state === CaptureState.kArchiving ? 'archiving' : 'starting';
    if (status.directory) {
      this.directory_ = status.directory;
    }
    this.exchangeCount_ = status.exchangeCount;
    this.bodyBytes_ = status.bodyBytes;
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
      default:
        return loadTimeData.getString('notArchiving');
    }
  }

  protected isCapturing_(): boolean {
    return this.state_ === 'starting' || this.state_ === 'archiving';
  }

  protected totalsText_(): string {
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
                           exchange.mimeType} ${exchange.url}`.toLowerCase();
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
    return exchange.statusCode ? String(exchange.statusCode) : '—';
  }

  protected statusTitle_(exchange: Exchange): string {
    return exchange.statusCode ? '' : loadTimeData.getString('requestOnly');
  }

  protected typeText_(exchange: Exchange): string {
    const subtype = exchange.mimeType.split('/')[1] || exchange.mimeType;
    return subtype.replace(/^x-/, '').replace(/\+.*$/, '');
  }

  protected sizeText_(exchange: Exchange): string {
    return (exchange.truncated ? '⚠ ' : '') + formatBytes(exchange.bodyBytes);
  }

  protected sizeTitle_(exchange: Exchange): string {
    return exchange.truncated ? loadTimeData.getString('truncated') : '';
  }

  // The URL without its scheme, which says little and takes room.
  protected urlText_(exchange: Exchange): string {
    return exchange.url.replace(/^https?:\/\//, '');
  }

  protected rowTitle_(exchange: Exchange): string {
    return `${exchange.method} ${exchange.url}\n${
        new Date(exchange.timeMs).toLocaleTimeString()}`;
  }

  protected isError_(exchange: Exchange): boolean {
    return exchange.statusCode === 0 || exchange.statusCode >= 400;
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
