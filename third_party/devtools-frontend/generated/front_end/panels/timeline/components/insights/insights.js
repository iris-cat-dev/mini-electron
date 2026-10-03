import*as e from"../../../../core/i18n/i18n.js";import*as t from"../../../../models/trace/trace.js";import*as s from"../../../../ui/lit-html/lit-html.js";import*as i from"../../../../core/platform/platform.js";import*as n from"../../../../ui/components/helpers/helpers.js";import*as o from"../../utils/utils.js";import"../../../../ui/components/markdown_view/markdown_view.js";import*as r from"../../../../third_party/marked/marked.js";import*as a from"../../../../ui/visual_logging/visual_logging.js";import"../../../../ui/components/buttons/buttons.js";import"../../../../ui/components/icon_button/icon_button.js";import*as l from"../../../../core/common/common.js";import*as d from"../../../../core/sdk/sdk.js";import"../../../../ui/components/linkifier/linkifier.js";const c=new CSSStyleSheet;c.replaceSync(":host{--markdown-link-text-decoration-style:underline}.insight{display:block;position:relative;width:auto;height:auto;margin:var(--sys-size-6) 0;border-radius:var(--sys-shape-corner-extra-small);overflow:hidden;border:var(--sys-size-1) solid var(--sys-color-divider);background-color:var(--sys-color-base);&.closed{background-color:var(--sys-color-surface3);border:none;&:focus-within{outline:var(--sys-size-1) solid var(--sys-color-primary)}}header{padding:var(--sys-size-5) var(--sys-size-6);h3{font:var(--sys-typescale-body4-medium)}}&:not(.closed){header{padding-bottom:var(--sys-size-2)}}}.insight-hover-icon{position:absolute;top:var(--sys-size-5);right:var(--sys-size-5);border:none;width:var(--sys-size-9);user-select:none;height:var(--sys-size-9);box-shadow:var(--sys-elevation-level1);border-radius:var(--sys-shape-corner-full);background:var(--sys-color-cdt-base-container);opacity:0%;transition:opacity 0.2s ease;.insight:hover &,\n  header:focus-within &{opacity:100%}devtools-button{transition:transform 0.2s ease}&.active devtools-button{transform:rotate(180deg)}}.insight-description,\n.insight-body,\n.insight-title{user-select:text}.insight-body{padding:0 var(--sys-size-6) var(--sys-size-5) var(--sys-size-6)}.insight-section{padding-top:var(--sys-size-5)}.insight-description:not(:empty){padding-bottom:var(--sys-size-5)}.insight-section:not(:empty){border-top:var(--sys-size-1) solid var(--sys-color-divider)}.insight-title{color:var(--sys-color-on-base);margin-block:3px}.link{color:var(--sys-color-primary)}.dl-title{font-weight:bold}dd.dl-title{text-align:right}.dl-value{font-weight:bold}.metric-value-bad{color:var(--app-color-performance-bad)}.metric-value-good{color:var(--app-color-performance-good)}.insight-entry{font:var(--sys-typescale-body4-medium);padding-block:var(--sys-size-3);display:flex;align-items:center}.element-img{width:var(--sys-size-13);height:var(--sys-size-13);object-fit:cover;border:var(--sys-size-1) solid var(--sys-color-divider);background:var(--sys-color-divider) -0.054px -12px /100.239% 148.936% no-repeat;margin-right:var(--sys-size-5)}.element-img-details{font:var(--sys-typescale-body4-regular);display:flex;flex-direction:column;word-break:break-all;.element-img-details-size{color:var(--color-text-secondary)}}::slotted(*){font:var(--sys-typescale-body4-regular)}.insight-savings{font:var(--sys-typescale-body4-medium);color:var(--sys-color-green)}ul.insight-icon-results{list-style:none;margin:0;padding:0;li{display:flex;align-items:flex-start;justify-content:flex-start;gap:var(--sys-size-3);span{padding-top:2px}}}.timeline-link{cursor:pointer;text-decoration:underline;color:var(--sys-color-primary);background:none;border:none;padding:0;font:inherit}.timeline-link.invalid-link{color:var(--sys-color-state-disabled)}.lcp-element{display:inline-flex;align-items:center}.insight-results:not(:last-child){border-bottom:var(--sys-size-1) solid var(--sys-color-divider);padding-bottom:var(--sys-size-5)}.lcp-element:not(:empty){padding:inherit}\n/*# sourceURL=sidebarInsight.css */\n");const{html:h}=s;class m extends Event{event;static eventName="eventreferenceclick";constructor(e){super(m.eventName,{bubbles:!0,composed:!0}),this.event=e}}class g extends HTMLElement{#e=this.attachShadow({mode:"open"});#t=this.#s.bind(this);#i=null;#n=null;connectedCallback(){this.#e.adoptedStyleSheets=[c]}set text(e){this.#i=e,n.ScheduledRender.scheduleRender(this,this.#t)}set event(e){this.#n=e,n.ScheduledRender.scheduleRender(this,this.#t)}#s(){this.#i&&this.#n&&s.render(h`
      <button type="button" class="timeline-link" @click=${e=>{e.stopPropagation(),this.#n&&this.dispatchEvent(new m(this.#n))}}>${this.#i}</button>
    `,this.#e,{host:this})}}function p(e){let s,n;return t.Types.Events.isSyntheticNetworkRequest(e)?(n=o.Helpers.shortenUrl(new URL(e.args.data.url)),s=e.args.data.url):i.TypeScriptUtilities.assertNever(e,`unsupported event in eventRef: ${e.name}`),h`<devtools-performance-event-ref
    .event=${e}
    .text=${n}
    title=${s}
  ></devtools-performance-event-ref>`}customElements.define("devtools-performance-event-ref",g);var v=Object.freeze({__proto__:null,EventReferenceClick:m,eventRef:p});const{html:u}=s,y={estimatedSavings:"Est savings: {PH1}",estimatedSavingsTimingAndBytes:"Est savings: {PH1} & {PH2}",viewDetails:"View details for {PH1}"},b=e.i18n.registerUIStrings("panels/timeline/components/insights/SidebarInsight.ts",y),f=e.i18n.getLocalizedString.bind(void 0,b);class S extends Event{model;insightSetKey;overlays;static eventName="insightactivated";constructor(e,t,s){super(S.eventName,{bubbles:!0,composed:!0}),this.model=e,this.insightSetKey=t,this.overlays=s}}class T extends Event{static eventName="insightdeactivated";constructor(){super(T.eventName,{bubbles:!0,composed:!0})}}class w extends Event{bounds;static eventName="insightsethovered";constructor(e){super(w.eventName,{bubbles:!0,composed:!0}),this.bounds=e}}class R extends Event{bounds;static eventName="insightsetzoom";constructor(e){super(R.eventName,{bubbles:!0,composed:!0}),this.bounds=e}}class C extends Event{overlays;options;static eventName="insightprovideoverlays";constructor(e,t){super(C.eventName,{bubbles:!0,composed:!0}),this.overlays=e,this.options=t}}class k extends Event{label;events;activateInsight;static eventName="insightproviderelatedevents";constructor(e,t,s){super(k.eventName,{bubbles:!0,composed:!0}),this.label=e,this.events=t,this.activateInsight=s}}class E extends HTMLElement{#e=this.attachShadow({mode:"open"});#t=this.#s.bind(this);#o="";#r="";#a="";#l=!1;#d=void 0;#c=void 0;set data(e){this.#o=e.title,this.#r=e.description,this.#a=e.internalName,this.#l=e.expanded,this.#d=e.estimatedSavingsTime,this.#c=e.estimatedSavingsBytes,this.dataset.insightTitle=e.title,e.expanded?this.dataset.insightExpanded="":delete this.dataset.insightExpanded,n.ScheduledRender.scheduleRender(this,this.#t)}connectedCallback(){this.#e.adoptedStyleSheets=[c],n.ScheduledRender.scheduleRender(this,this.#t)}#h(){this.dispatchEvent(new CustomEvent("insighttoggleclick"))}#m(e){const t=s.Directives.classMap({"insight-hover-icon":!0,active:e});return u`
      <div class=${t} inert>
        <devtools-button .data=${{variant:"icon",iconName:"chevron-down",size:"SMALL"}}
      ></devtools-button>
      </div>

    `}#g(e){"Enter"!==e.key&&" "!==e.key||(e.preventDefault(),e.stopPropagation(),this.#h())}#p(){let t,s;return void 0!==this.#d&&this.#d>0&&(t=e.TimeUtilities.millisToString(this.#d)),void 0!==this.#c&&this.#c>0&&(s=e.ByteUtilities.bytesToString(this.#c)),t&&s?f(y.estimatedSavingsTimingAndBytes,{PH1:t,PH2:s}):t?f(y.estimatedSavings,{PH1:t}):s?f(y.estimatedSavings,{PH1:s}):null}#s(){const e=s.Directives.classMap({insight:!0,closed:!this.#l}),t=this.#p(),i=u`
      <div class=${e}>
        <header @click=${this.#h}
          @keydown=${this.#g}
          jslog=${a.action(`timeline.toggle-insight.${this.#a}`).track({click:!0})}
          tabIndex="0"
          role="button"
          aria-expanded=${this.#l}
          aria-label=${f(y.viewDetails,{PH1:this.#o})}
        >
          ${this.#m(this.#l)}
          <h3 class="insight-title">${this.#o}</h3>
          ${t?u`
            <slot name="insight-savings" class="insight-savings">
              ${t}
            </slot>
          </div>`:s.nothing}
        </header>
        ${this.#l?u`
          <div class="insight-body">
            <div class="insight-description">${this.#r?x(this.#r):s.nothing}</div>
            <div class="insight-content">
              <slot name="insight-content"></slot>
            </div>
          </div>`:s.nothing}
      </div>
    `;s.render(i,this.#e,{host:this})}}customElements.define("devtools-performance-sidebar-insight",E);var L,N=Object.freeze({__proto__:null,InsightActivated:S,InsightDeactivated:T,InsightProvideOverlays:C,InsightProvideRelatedEvents:k,InsightSetHovered:w,InsightSetZoom:R,SidebarInsight:E});!function(e){e.ALL="All",e.INP="INP",e.LCP="LCP",e.CLS="CLS"}(L||(L={}));var $=Object.freeze({__proto__:null,get Category(){return L}});const{html:I}=s;function D(e){return e.activeCategory===L.ALL||e.activeCategory===e.insightCategory}class _ extends HTMLElement{static litTagName=s.literal``;shadow=this.attachShadow({mode:"open"});#v=!1;#u=null;get model(){return this.#u}data={parsedTrace:null,insightSetKey:null,activeCategory:L.ALL};#t=this.#y.bind(this);sharedTableState={selectedRowEl:null,selectionIsSticky:!1};#b=null;#f=!1;scheduleRender(){n.ScheduledRender.scheduleRender(this,this.#t)}connectedCallback(){if(this.shadow.adoptedStyleSheets.push(c),this.setAttribute("jslog",`${a.section(`timeline.insights.${this.internalName}`)}`),this.dataset.insightName=this.internalName,!this.#f&&this.#u){this.#f=!0;const e=this.#u.relatedEvents??[];e.length&&this.dispatchEvent(new k(this.#u.title,e,this.#S.bind(this)))}}set selected(e){this.#v=e,n.ScheduledRender.scheduleRender(this,this.#t)}set model(e){this.#u=e,n.ScheduledRender.scheduleRender(this,this.#t)}set parsedTrace(e){this.data.parsedTrace=e,n.ScheduledRender.scheduleRender(this,this.#t)}set insightSetKey(e){this.data.insightSetKey=e,n.ScheduledRender.scheduleRender(this,this.#t)}set activeCategory(e){this.data.activeCategory=e,n.ScheduledRender.scheduleRender(this,this.#t)}onSidebarClick(){this.isActive()?this.dispatchEvent(new T):this.#S()}#S(){this.data.insightSetKey&&(this.sharedTableState.selectedRowEl?.classList.remove("selected"),this.sharedTableState.selectedRowEl=null,this.sharedTableState.selectionIsSticky=!1,this.dispatchEvent(new S(this.model,this.data.insightSetKey,this.getInitialOverlays())))}toggleTemporaryOverlays(e,t){this.isActive()&&this.dispatchEvent(new C(e??this.getInitialOverlays(),t))}getInitialOverlays(){return this.#b||(this.#b=this.createOverlays()),this.#b}#y(){this.render(),this.isActive()&&requestAnimationFrame((()=>requestAnimationFrame((()=>this.scrollIntoViewIfNeeded()))))}isActive(){return this.#v}}function x(e){const t=r.Marked.lexer(e);return I`<devtools-markdown-view .data=${{tokens:t}}></devtools-markdown-view>`}var P=Object.freeze({__proto__:null,BaseInsightComponent:_,md:x,shouldRenderForCategory:D});const{html:M}=s,A={worstLayoutShiftCluster:"Worst layout shift cluster",worstCluster:"Worst cluster",layoutShiftCluster:"Layout shift cluster @ {PH1}",topCulprits:"Top layout shift culprits",injectedIframe:"Injected iframe",fontRequest:"Font request",animation:"Animation",unsizedImages:"Unsized Images"},z=e.i18n.registerUIStrings("panels/timeline/components/insights/CLSCulprits.ts",A),O=e.i18n.getLocalizedString.bind(void 0,z);class H extends _{static litTagName=s.literal`devtools-performance-cls-culprits`;insightCategory=L.CLS;internalName="cls-culprits";createOverlays(){const e=(this.model?.clusters.toSorted(((e,t)=>t.clusterCumulativeScore-e.clusterCumulativeScore))??[])[0];if(!e)return[];const s=t.Types.Timing.MicroSeconds(e.dur??0),i=t.Types.Timing.MicroSeconds(e.ts+s),n=M`<div>${O(A.worstLayoutShiftCluster)}</div>`;return[{type:"TIMESPAN_BREAKDOWN",sections:[{bounds:{min:e.ts,range:s,max:i},label:n,showDuration:!1}],entry:e.events[0],renderLocation:"ABOVE_EVENT"}]}getTopCulprits(e,t){const s=[];if(3===s.length)return s;const i=e.events;for(const e of i){if(3===s.length)break;const i=t.get(e);if(!i)continue;const n=i.fontRequests,o=i.iframeIds,r=i.nonCompositedAnimations,a=i.unsizedImages;for(let e=0;e<n.length&&s.length<3;e++)s.push(O(A.fontRequest));for(let e=0;e<o.length&&s.length<3;e++)s.push(O(A.injectedIframe));for(let e=0;e<r.length&&s.length<3;e++)s.push(O(A.animation));for(let e=0;e<a.length&&s.length<3;e++)s.push(O(A.unsizedImages))}return s.slice(0,3)}#T(e){this.dispatchEvent(new m(e))}#s(i,n){if(!this.model)return s.nothing;const o=t.Types.Timing.MicroSeconds(n.ts-(this.data.parsedTrace?.Meta.traceBounds.min??0)),r=e.TimeUtilities.formatMicroSecondsTime(o);return M`
        <div class="insights">
            <devtools-performance-sidebar-insight .data=${{title:this.model.title,description:this.model.description,internalName:this.internalName,expanded:this.isActive()}}
            @insighttoggleclick=${this.onSidebarClick}>
                <div slot="insight-content" class="insight-section">
                  <span class="worst-cluster">${O(A.worstCluster)}: <button type="button" class="timeline-link" @click=${()=>this.#T(n)}>${O(A.layoutShiftCluster,{PH1:r})}</button></span>
                    <p>${O(A.topCulprits)}:</p>
                        ${i.map((e=>M`
                            <li>${e}</li>
                          `))}
                </div>
            </devtools-performance-sidebar-insight>
        </div>`}render(){if(!this.model)return;const e=this.model.shifts;if(!(this.model.clusters??[]).length||!this.model.worstCluster)return;const t=this.getTopCulprits(this.model.worstCluster,e),i=t.length>0,n=D({activeCategory:this.data.activeCategory,insightCategory:this.insightCategory}),o=i&&n?this.#s(t,this.model.worstCluster):s.nothing;s.render(o,this.shadow,{host:this})}}customElements.define("devtools-performance-cls-culprits",H);var F=Object.freeze({__proto__:null,CLSCulprits:H});const{html:q}=s,B={passingRedirects:"Avoids redirects",failedRedirects:"Had redirects",passingServerResponseTime:"Server responds quickly",failedServerResponseTime:"Server responded slowly",passingTextCompression:"Applies text compression",failedTextCompression:"No compression applied",redirectsLabel:"Redirects",serverResponseTimeLabel:"Server response time",uncompressedDownload:"Uncompressed download",successAriaLabel:"Insight check passed: {PH1}",failedAriaLabel:"Insight check failed: {PH1}"},j=e.i18n.registerUIStrings("panels/timeline/components/insights/DocumentLatency.ts",B),U=e.i18n.getLocalizedString.bind(void 0,j);class W extends _{static litTagName=s.literal`devtools-performance-document-latency`;insightCategory=L.ALL;internalName="document-latency";#w(e,t,s){const i=e?"check-circle":"clear",n=e?U(B.successAriaLabel,{PH1:t}):U(B.failedAriaLabel,{PH1:s});return q`
      <devtools-icon
        aria-label=${n}
        name=${i}
        class=${e?"metric-value-good":"metric-value-bad"}
      ></devtools-icon>
      <span>${e?t:s}</span>
    `}createOverlays(){if(!this.model?.data?.documentRequest)return[];const e=[],s=this.model.data.documentRequest,i=t.Helpers.Timing.millisecondsToMicroseconds(this.model.data.redirectDuration),n=[];if(this.model.data.redirectDuration){const o=t.Helpers.Timing.traceWindowFromMicroSeconds(s.ts,s.ts+i);n.push({bounds:o,label:U(B.redirectsLabel),showDuration:!0}),e.push({type:"CANDY_STRIPED_TIME_RANGE",bounds:o,entry:s})}if(this.model.data.serverResponseTooSlow){const e=t.Helpers.Timing.millisecondsToMicroseconds(this.model.data.serverResponseTime),i=s.args.data.timing?.sendEnd??t.Types.Timing.MilliSeconds(0),o=t.Helpers.Timing.millisecondsToMicroseconds(i),r=t.Helpers.Timing.traceWindowFromMicroSeconds(o,o+e);n.push({bounds:r,label:U(B.serverResponseTimeLabel),showDuration:!0})}if(this.model.data.uncompressedResponseBytes){const i=t.Helpers.Timing.traceWindowFromMicroSeconds(s.args.data.syntheticData.downloadStart,s.args.data.syntheticData.downloadStart+s.args.data.syntheticData.download);n.push({bounds:i,label:U(B.uncompressedDownload),showDuration:!0}),e.push({type:"CANDY_STRIPED_TIME_RANGE",bounds:i,entry:s})}return n.length&&e.push({type:"TIMESPAN_BREAKDOWN",sections:n,entry:this.model.data.documentRequest,renderLocation:"BELOW_EVENT"}),e.push({type:"ENTRY_SELECTED",entry:this.model.data.documentRequest}),e}#R(){return this.model?.data?q`
    <div class="insights">
      <devtools-performance-sidebar-insight .data=${{title:this.model.title,description:this.model.description,expanded:this.isActive(),internalName:this.internalName,estimatedSavingsTime:this.model.metricSavings?.FCP,estimatedSavingsBytes:this.model.data.uncompressedResponseBytes}}
        @insighttoggleclick=${this.onSidebarClick}
      >
        <div slot="insight-content" class="insight-section">
          <ul class="insight-results insight-icon-results">
            <li class="insight-entry">
              ${this.#w(0===this.model.data.redirectDuration,U(B.passingRedirects),U(B.failedRedirects))}
            </li>
            <li class="insight-entry">
              ${this.#w(!this.model.data.serverResponseTooSlow,U(B.passingServerResponseTime),U(B.failedServerResponseTime))}
            </li>
            <li class="insight-entry">
              ${this.#w(0===this.model.data.uncompressedResponseBytes,U(B.passingTextCompression),U(B.failedTextCompression))}
            </li>
          </ul>
        </div>
      </devtools-performance-sidebar-insight>
    </div>`:s.nothing}render(){if(void 0===this.model?.data)return;const e=D({activeCategory:this.data.activeCategory,insightCategory:this.insightCategory}),t=this.model?.data?.redirectDuration>0||this.model?.data?.serverResponseTooSlow||this.model.data.uncompressedResponseBytes>0,i=e&&t?this.#R():s.nothing;s.render(i,this.shadow,{host:this})}}customElements.define("devtools-performance-document-latency",W);var K=Object.freeze({__proto__:null,DocumentLatency:W});const V=new CSSStyleSheet;V.replaceSync('table{width:100%;padding:5px 0;border-collapse:collapse}thead{white-space:nowrap}table tr > *{text-align:right}table tr > *:first-child{text-align:left}table.interactive tbody tr{cursor:pointer}table.interactive tbody tr:hover,\ntable.interactive tbody tr.selected{background-color:var(--app-color-performance-sidebar-table-row-hover)}table thead th{font:var(--sys-typescale-body4-medium)}table tbody th{font-weight:normal}table th[scope="row"]{padding:3px 0;word-break:break-word}\n/*# sourceURL=table.css */\n');const{html:Y}=s;class G extends HTMLElement{#e=this.attachShadow({mode:"open"});#t=this.#s.bind(this);#C;#k;#E;#L;#N=!1;#$=null;set data(e){this.#C=e.insight,this.#k=e.insight.sharedTableState,this.#E=e.headers,this.#L=e.rows,this.#N=this.#L.some((e=>e.overlays)),n.ScheduledRender.scheduleRender(this,this.#t)}connectedCallback(){this.#e.adoptedStyleSheets.push(V),n.ScheduledRender.scheduleRender(this,this.#t)}#I(e){if(!(e.target instanceof HTMLElement))return;const t=e.target.closest("tr");if(!t||!t.parentElement)return;const s=[...t.parentElement.children].indexOf(t);-1!==s&&s!==this.#$&&(this.#$=s,this.#D(t,s,{isHover:!0}))}#_(e){if(!(e.target instanceof HTMLElement))return;const t=e.target.closest("tr");if(!t||!t.parentElement)return;const s=[...t.parentElement.children].indexOf(t);-1!==s&&this.#D(t,s,{sticky:!0})}#x(){this.#$=null,this.#D(null,null)}#D(e,t,s={}){if(this.#L&&this.#k&&this.#C&&(!this.#k.selectionIsSticky||s.sticky)){if(this.#k.selectionIsSticky&&e===this.#k.selectedRowEl&&(e=null,s.sticky=!1),e&&null!==t){const e=this.#L[t].overlays;e&&this.#C.toggleTemporaryOverlays(e,{updateTraceWindow:!s.isHover})}else this.#C.toggleTemporaryOverlays(null,{updateTraceWindow:!1});this.#k.selectedRowEl?.classList.remove("selected"),e?.classList.add("selected"),this.#k.selectedRowEl=e,this.#k.selectionIsSticky=s.sticky??!1}}async#s(){this.#E&&this.#L&&s.render(Y`<table
          class=${s.Directives.classMap({interactive:this.#N})}
          @mouseleave=${this.#N?this.#x:null}>
        <thead>
          <tr>
          ${this.#E.map((e=>Y`<th scope="col">${e}</th>`))}
          </tr>
        </thead>
        <tbody
          @mouseover=${this.#N?this.#I:null}
          @click=${this.#N?this.#_:null}
        >
          ${this.#L.map((e=>{const t=e.values.map(((e,t)=>0===t?Y`<th scope="row">${e}</th>`:Y`<td>${e}</td>`));return Y`<tr>${t}</tr>`}))}
        </tbody>
      </table>`,this.#e,{host:this})}}customElements.define("devtools-performance-table",G);var J=Object.freeze({__proto__:null,Table:G});const{html:Z}=s,Q={fontColumn:"Font",wastedTimeColumn:"Wasted time"},X=e.i18n.registerUIStrings("panels/timeline/components/insights/FontDisplay.ts",Q),ee=e.i18n.getLocalizedString.bind(void 0,X);class te extends _{static litTagName=s.literal`devtools-performance-font-display`;insightCategory=L.INP;internalName="font-display";#P=new Map;createOverlays(){if(this.#P.clear(),!this.model)return[];for(const e of this.model.fonts)this.#P.set(e.request,{type:"ENTRY_OUTLINE",entry:e.request,outlineReason:e.wastedTime?"ERROR":"INFO"});return[...this.#P.values()]}#s(t){return this.model?Z`
        <div class="insights">
            <devtools-performance-sidebar-insight .data=${{title:this.model.title,description:this.model.description,expanded:this.isActive(),internalName:this.internalName,estimatedSavingsTime:t.metricSavings?.FCP}}
            @insighttoggleclick=${this.onSidebarClick}>
                <div slot="insight-content" class="insight-section">
                  ${Z`<devtools-performance-table
                    .data=${{insight:this,headers:[ee(Q.fontColumn),"font-display",ee(Q.wastedTimeColumn)],rows:t.fonts.map((t=>({values:[p(t.request),t.display,e.TimeUtilities.millisToString(t.wastedTime)],overlays:[this.#P.get(t.request)]})))}}>
                  </devtools-performance-table>`}
                </div>
            </devtools-performance-sidebar-insight>
        </div>`:s.nothing}render(){const e=this.model,t=e&&e.fonts.find((e=>e.wastedTime)),i=D({activeCategory:this.data.activeCategory,insightCategory:this.insightCategory}),n=t&&i?this.#s(e):s.nothing;s.render(n,this.shadow,{host:this})}}customElements.define("devtools-performance-font-display",te);var se=Object.freeze({__proto__:null,FontDisplay:te});const{html:ie}=s,ne={phase:"Phase",duration:"Duration",inputDelay:"Input delay",processingDuration:"Processing duration",presentationDelay:"Presentation delay"},oe=e.i18n.registerUIStrings("panels/timeline/components/insights/InteractionToNextPaint.ts",ne),re=e.i18n.getLocalizedString.bind(void 0,oe);class ae extends _{static litTagName=s.literal`devtools-performance-inp`;insightCategory=L.INP;internalName="inp";createOverlays(){if(!this.model)return[];const e=this.model.longestInteractionEvent;return e?this.#M(e):[]}#M(e,s=-1){const i=t.Helpers.Timing.traceWindowFromMicroSeconds(e.ts,e.ts+e.inputDelay),n=t.Helpers.Timing.traceWindowFromMicroSeconds(i.max,i.max+e.mainThreadHandling),o=t.Helpers.Timing.traceWindowFromMicroSeconds(n.max,n.max+e.presentationDelay);let r=[{bounds:i,label:re(ne.inputDelay),showDuration:!0},{bounds:n,label:re(ne.processingDuration),showDuration:!0},{bounds:o,label:re(ne.presentationDelay),showDuration:!0}];return-1!==s&&(r=[r[s]]),[{type:"TIMESPAN_BREAKDOWN",sections:r,renderLocation:"BELOW_EVENT",entry:e}]}#s(t){if(!this.model)return s.nothing;const n=t=>e.TimeUtilities.millisToString(i.Timing.microSecondsToMilliSeconds(t));return ie`
        <div class="insights">
            <devtools-performance-sidebar-insight .data=${{title:this.model.title,description:this.model.description,internalName:this.internalName,expanded:this.isActive()}}
            @insighttoggleclick=${this.onSidebarClick}>
                <div slot="insight-content" class="insight-section">
                  ${ie`<devtools-performance-table
                    .data=${{insight:this,headers:[re(ne.phase),re(ne.duration)],rows:[{values:[re(ne.inputDelay),n(t.inputDelay)],overlays:this.#M(t,0)},{values:[re(ne.processingDuration),n(t.mainThreadHandling)],overlays:this.#M(t,1)},{values:[re(ne.presentationDelay),n(t.presentationDelay)],overlays:this.#M(t,2)}]}}>
                  </devtools-performance-table>`}
                </div>
            </devtools-performance-sidebar-insight>
        </div>`}render(){const e=this.model?.longestInteractionEvent,t=D({activeCategory:this.data.activeCategory,insightCategory:this.insightCategory}),i=e&&t?this.#s(e):s.nothing;s.render(i,this.shadow,{host:this})}}customElements.define("devtools-performance-inp",ae);var le=Object.freeze({__proto__:null,InteractionToNextPaint:ae});const{html:de}=s,ce={lcpLoadDelay:"LCP image loaded {PH1} after earliest start point.",fetchPriorityApplied:"fetchpriority=high applied",requestDiscoverable:"Request is discoverable in initial document",lazyLoadNotApplied:"lazy load not applied",successAriaLabel:"Insight check passed: {PH1}",failedAriaLabel:"Insight check failed: {PH1}"},he=e.i18n.registerUIStrings("panels/timeline/components/insights/LCPDiscovery.ts",ce),me=e.i18n.getLocalizedString.bind(void 0,he);function ge(e){if(!e)return null;if(void 0===e.lcpRequest)return null;const s=e.shouldIncreasePriorityHint,i=e.shouldPreloadImage,n=e.shouldRemoveLazyLoading;if(!(void 0!==s&&void 0!==i&&void 0!==n))return null;const o={shouldIncreasePriorityHint:s,shouldPreloadImage:i,shouldRemoveLazyLoading:n,request:e.lcpRequest,discoveryDelay:null,estimatedSavings:e.metricSavings?.LCP??null};if(e.earliestDiscoveryTimeTs&&e.lcpRequest){const s=e.lcpRequest.ts-e.earliestDiscoveryTimeTs;o.discoveryDelay=t.Types.Timing.MicroSeconds(s)}return o}class pe extends _{static litTagName=s.literal`devtools-performance-lcp-discovery`;insightCategory=L.LCP;internalName="lcp-discovery";#A(e,t){const s=e?"clear":"check-circle",i=me(e?ce.failedAriaLabel:ce.successAriaLabel,{PH1:t});return de`
      <devtools-icon
        aria-label=${i}
        name=${s}
        class=${e?"metric-value-bad":"metric-value-good"}
      ></devtools-icon>
    `}#z(t){const s=document.createElement("span");return s.classList.add("discovery-time-ms"),s.innerText=e.TimeUtilities.formatMicroSecondsTime(t),e.i18n.getFormatLocalizedString(he,ce.lcpLoadDelay,{PH1:s})}createOverlays(){const e=ge(this.model);if(!e||!e.discoveryDelay)return[];const s=t.Helpers.Timing.traceWindowFromMicroSeconds(t.Types.Timing.MicroSeconds(e.request.ts-e.discoveryDelay),e.request.ts),i=de`<div class="discovery-delay"> ${this.#z(s.range)}</div>`;return[{type:"ENTRY_OUTLINE",entry:e.request,outlineReason:"ERROR"},{type:"CANDY_STRIPED_TIME_RANGE",bounds:s,entry:e.request},{type:"TIMESPAN_BREAKDOWN",sections:[{bounds:s,label:i,showDuration:!1}],entry:e.request,renderLocation:"ABOVE_EVENT"}]}#O(e){e.target.style.display="none"}#H(t){return de`
      <div class="lcp-element">
        ${t.request.args.data.mimeType.includes("image")?de`
        <img
          class="element-img"
          src=${t.request.args.data.url}
          @error=${this.#O}
           />`:s.nothing}
        <span class="element-img-details">
          ${p(t.request)}
          <span class="element-img-details-size">${e.ByteUtilities.bytesToString(t.request.args.data.decodedBodyLength??0)}</span>
        </span>
      </div>`}#F(e){return this.model?de`
        <div class="insights">
          <devtools-performance-sidebar-insight .data=${{title:this.model.title,description:this.model.description,internalName:this.internalName,expanded:this.isActive(),estimatedSavingsTime:e.estimatedSavings}}
          @insighttoggleclick=${this.onSidebarClick}>
            <div slot="insight-content" class="insight-section">
              <div class="insight-results">
                <ul class="insight-icon-results">
                  <li class="insight-entry">
                    ${this.#A(e.shouldIncreasePriorityHint,me(ce.fetchPriorityApplied))}
                    <span>${me(ce.fetchPriorityApplied)}</span>
                  </li>
                  <li class="insight-entry">
                    ${this.#A(e.shouldPreloadImage,me(ce.requestDiscoverable))}
                    <span>${me(ce.requestDiscoverable)}</span>
                  </li>
                  <li class="insight-entry">
                    ${this.#A(e.shouldRemoveLazyLoading,me(ce.lazyLoadNotApplied))}
                    <span>${me(ce.lazyLoadNotApplied)}</span>
                  </li>
                </ul>
              </div>
              ${this.#H(e)}
            </div>
          </devtools-performance-sidebar-insight>
      </div>`:s.nothing}render(){const e=ge(this.model),t=D({activeCategory:this.data.activeCategory,insightCategory:this.insightCategory}),i=e&&t?this.#F(e):s.nothing;s.render(i,this.shadow,{host:this})}}customElements.define("devtools-performance-lcp-discovery",pe);var ve=Object.freeze({__proto__:null,LCPDiscovery:pe});const{html:ue}=s,ye={timeToFirstByte:"Time to first byte",resourceLoadDelay:"Resource load delay",resourceLoadDuration:"Resource load duration",elementRenderDelay:"Element render delay",phase:"Phase",percentLCP:"% of LCP"},be=e.i18n.registerUIStrings("panels/timeline/components/insights/LCPPhases.ts",ye),fe=e.i18n.getLocalizedString.bind(void 0,be);class Se extends _{static litTagName=s.literal`devtools-performance-lcp-by-phases`;insightCategory=L.LCP;internalName="lcp-by-phase";#q=null;#B(){if(!this.model)return[];const e=this.model.lcpMs,t=this.model.phases;if(!e||!t)return[];const{ttfb:s,loadDelay:i,loadTime:n,renderDelay:o}=t;if(i&&n){return[{phase:fe(ye.timeToFirstByte),timing:s,percent:`${(100*s/e).toFixed(0)}%`},{phase:fe(ye.resourceLoadDelay),timing:i,percent:`${(100*i/e).toFixed(0)}%`},{phase:fe(ye.resourceLoadDuration),timing:n,percent:`${(100*n/e).toFixed(0)}%`},{phase:fe(ye.elementRenderDelay),timing:o,percent:`${(100*o/e).toFixed(0)}%`}]}return[{phase:fe(ye.timeToFirstByte),timing:s,percent:`${(100*s/e).toFixed(0)}%`},{phase:fe(ye.elementRenderDelay),timing:o,percent:`${(100*o/e).toFixed(0)}%`}]}createOverlays(){if(this.#q=null,!this.model)return[];const e=this.model.phases,s=this.model.lcpTs;if(!e||!s)return[];const i=t.Types.Timing.MicroSeconds(t.Helpers.Timing.millisecondsToMicroseconds(s)),n=[];this.model.lcpRequest&&n.push({type:"ENTRY_OUTLINE",entry:this.model.lcpRequest,outlineReason:"INFO"});const o=[];if(e?.loadDelay||e?.loadTime){if(e?.loadDelay&&e?.loadTime){const s=t.Types.Timing.MicroSeconds(i-t.Helpers.Timing.millisecondsToMicroseconds(e.renderDelay)),n=t.Helpers.Timing.traceWindowFromMicroSeconds(s,i),r=t.Types.Timing.MicroSeconds(s-t.Helpers.Timing.millisecondsToMicroseconds(e.loadTime)),a=t.Helpers.Timing.traceWindowFromMicroSeconds(r,s),l=t.Types.Timing.MicroSeconds(r-t.Helpers.Timing.millisecondsToMicroseconds(e.loadDelay)),d=t.Helpers.Timing.traceWindowFromMicroSeconds(l,r),c=t.Types.Timing.MicroSeconds(l-t.Helpers.Timing.millisecondsToMicroseconds(e.ttfb)),h=t.Helpers.Timing.traceWindowFromMicroSeconds(c,l);o.push({bounds:h,label:fe(ye.timeToFirstByte),showDuration:!0},{bounds:d,label:fe(ye.resourceLoadDelay),showDuration:!0},{bounds:a,label:fe(ye.resourceLoadDuration),showDuration:!0},{bounds:n,label:fe(ye.elementRenderDelay),showDuration:!0})}}else{const s=t.Types.Timing.MicroSeconds(i-t.Helpers.Timing.millisecondsToMicroseconds(e.renderDelay)),n=t.Helpers.Timing.traceWindowFromMicroSeconds(s,i),r=t.Types.Timing.MicroSeconds(s-t.Helpers.Timing.millisecondsToMicroseconds(e.ttfb)),a=t.Helpers.Timing.traceWindowFromMicroSeconds(r,s);o.push({bounds:a,label:fe(ye.timeToFirstByte),showDuration:!0},{bounds:n,label:fe(ye.elementRenderDelay),showDuration:!0})}return this.#q={type:"TIMESPAN_BREAKDOWN",sections:o},n.push(this.#q),n}#j(e){if(!this.model)return s.nothing;const t=e.map((({phase:e,percent:t})=>{const s=this.#q?.sections.find((t=>e===t.label));return{values:[e,t],overlays:s&&[{type:"TIMESPAN_BREAKDOWN",sections:[s]}]}}));return ue`
    <div class="insights">
      <devtools-performance-sidebar-insight .data=${{title:this.model.title,description:this.model.description,internalName:this.internalName,expanded:this.isActive()}}
        @insighttoggleclick=${this.onSidebarClick}
      >
        <div slot="insight-content" class="insight-section">
          ${ue`<devtools-performance-table
            .data=${{insight:this,headers:[fe(ye.phase),fe(ye.percentLCP)],rows:t}}>
          </devtools-performance-table>`}
        </div>
      </devtools-performance-sidebar-insight>
    </div>`}#U(e){return!!e&&e.length>0}render(){const e=this.#B(),t=D({activeCategory:this.data.activeCategory,insightCategory:this.insightCategory})&&this.#U(e)?this.#j(e):s.nothing;s.render(t,this.shadow,{host:this})}}customElements.define("devtools-performance-lcp-by-phases",Se);var Te=Object.freeze({__proto__:null,LCPPhases:Se});const{html:we}=s;class Re extends HTMLElement{#e=this.attachShadow({mode:"open"});#t=this.#s.bind(this);#W;#K;set data(e){this.#W=e.backendNodeId,this.#K=e.options,n.ScheduledRender.scheduleRender(this,this.#t)}async#V(){if(void 0===this.#W)return;const e=d.TargetManager.TargetManager.instance().primaryPageTarget();if(!e)return;const t=e.model(d.DOMModel.DOMModel);if(!t)return;const s=new Set([this.#W]),i=await t.pushNodesByBackendIdsToFrontend(s);if(!i)return;const n=i.get(this.#W);return n?l.Linkifier.Linkifier.linkify(n,this.#K):void 0}async#s(){const e=await this.#V();s.render(we`<div class='node-link'>
        ${e}
      </div>`,this.#e,{host:this})}}customElements.define("devtools-performance-node-link",Re);var Ce=Object.freeze({__proto__:null,NodeLink:Re});const{html:ke}=s,Ee={renderBlockingRequest:"Request",duration:"Duration"},Le=e.i18n.registerUIStrings("panels/timeline/components/insights/RenderBlocking.ts",Ee),Ne=e.i18n.getLocalizedString.bind(void 0,Le);class $e extends _{static litTagName=s.literal`devtools-performance-render-blocking-requests`;insightCategory=L.LCP;internalName="render-blocking-requests";createOverlays(){return this.model?this.model.renderBlockingRequests.map((e=>this.#Y(e))):[]}#Y(e){return{type:"ENTRY_OUTLINE",entry:e,outlineReason:"ERROR"}}#G(t){if(!this.model)return s.nothing;const n=t.metricSavings?.FCP,o=t.renderBlockingRequests.slice(0,3);return ke`
        <div class="insights">
          <devtools-performance-sidebar-insight .data=${{title:this.model.title,description:this.model.description,internalName:this.internalName,expanded:this.isActive(),estimatedSavingsTime:n}}
          @insighttoggleclick=${this.onSidebarClick} >
            <div slot="insight-content" class="insight-section">
              ${ke`<devtools-performance-table
                .data=${{insight:this,headers:[Ne(Ee.renderBlockingRequest),Ne(Ee.duration)],rows:o.map((t=>({values:[p(t),e.TimeUtilities.millisToString(i.Timing.microSecondsToMilliSeconds(t.dur))],overlays:[this.#Y(t)]})))}}>
              </devtools-performance-table>`}
            </div>
          </devtools-performance-sidebar-insight>
      </div>`}render(){const e=this.model,t=e?.renderBlockingRequests&&e.renderBlockingRequests.length>0,i=D({activeCategory:this.data.activeCategory,insightCategory:this.insightCategory}),n=t&&i?this.#G(e):s.nothing;s.render(n,this.shadow,{host:this})}}customElements.define("devtools-performance-render-blocking-requests",$e);var Ie=Object.freeze({__proto__:null,RenderBlocking:$e});const{html:De}=s,_e={matchAttempts:"Match attempts",matchCount:"Match count",elapsed:"Elapsed time",topSelectors:"Top selectors",total:"Total"},xe=e.i18n.registerUIStrings("panels/timeline/components/insights/SlowCSSSelector.ts",_e),Pe=e.i18n.getLocalizedString.bind(void 0,xe);class Me extends _{static litTagName=s.literal`devtools-performance-slow-css-selector`;insightCategory=L.ALL;internalName="slow-css-selector";#J=new Map;createOverlays(){return[]}async toSourceFileLocation(e,t){if(!e)return;const s=e.styleSheetHeaderForId(t.style_sheet_id);if(!s||!s.resourceURL())return;const i=JSON.stringify({selectorText:t.selector,styleSheetId:t.style_sheet_id});let n=this.#J.get(i);if(!n){const s=await e.agent.invoke_getLocationForSelector({selectorText:t.selector,styleSheetId:t.style_sheet_id});if(s.getError()||!s.ranges)return;n=s.ranges,this.#J.set(i,n)}return n.map(((e,t)=>({url:s.resourceURL(),lineNumber:e.startLine,columnNumber:e.startColumn,linkText:`[${t+1}]`,title:`${s.id} line ${e.startLine+1}:${e.startColumn+1}`})))}async getSelectorLinks(e,t){if(!e)return s.nothing;if(!t.style_sheet_id)return s.nothing;const i=await this.toSourceFileLocation(e,t);if(!i)return s.nothing;return De`
    ${i.map(((e,t)=>{const s=t!==i.length-1?", ":"";return De`<devtools-linkifier .data=${e}></devtools-linkifier>${s}`}))}`}renderSlowCSSSelector(){if(!this.model)return s.nothing;const n=d.TargetManager.TargetManager.instance().primaryPageTarget(),o=n?.model(d.CSSModel.CSSModel);return De`
      <div class="insights">
        <devtools-performance-sidebar-insight .data=${{title:this.model.title,description:this.model.description,internalName:this.internalName,expanded:this.isActive()}}
          @insighttoggleclick=${this.onSidebarClick} >
          <div slot="insight-content">
            <div class="insight-section">
              ${De`<devtools-performance-table
                .data=${{insight:this,headers:[Pe(_e.total),""],rows:[{values:[Pe(_e.elapsed),e.TimeUtilities.millisToString(this.model.totalElapsedMs)]},{values:[Pe(_e.matchAttempts),this.model.totalMatchAttempts]},{values:[Pe(_e.matchCount),this.model.totalMatchCount]}]}}>
              </devtools-performance-table>`}
            </div>
            <div class="insight-section">
              ${De`<devtools-performance-table
                .data=${{insight:this,headers:[Pe(_e.topSelectors),Pe(_e.elapsed)],rows:this.model.topElapsedMs.map((n=>{return{values:[De`${n.selector} ${s.Directives.until(this.getSelectorLinks(o,n))}`,(r=t.Types.Timing.MicroSeconds(n["elapsed (us)"]),e.TimeUtilities.millisToString(i.Timing.microSecondsToMilliSeconds(r)))]};var r}))}}>
              </devtools-performance-table>`}
            </div>
            <div class="insight-section">
              ${De`<devtools-performance-table
                .data=${{insight:this,headers:[Pe(_e.topSelectors),Pe(_e.matchAttempts)],rows:this.model.topMatchAttempts.map((e=>({values:[De`${e.selector} ${s.Directives.until(this.getSelectorLinks(o,e))}`,e.match_attempts]})))}}>
              </devtools-performance-table>`}
            </div>
          </div>
        </devtools-performance-sidebar-insight>
      </div>`}#U(){return null!==this.model&&0!==this.model.topElapsedMs.length&&0!==this.model.topMatchAttempts.length}render(){const e=D({activeCategory:this.data.activeCategory,insightCategory:this.insightCategory})&&this.#U()?this.renderSlowCSSSelector():s.nothing;s.render(e,this.shadow,{host:this})}}customElements.define("devtools-performance-slow-css-selector",Me);var Ae=Object.freeze({__proto__:null,SlowCSSSelector:Me});const{html:ze}=s,Oe={columnThirdParty:"Third party",columnTransferSize:"Transfer size",columnBlockingTime:"Blocking time"},He=e.i18n.registerUIStrings("panels/timeline/components/insights/ThirdParties.ts",Oe),Fe=e.i18n.getLocalizedString.bind(void 0,He);class qe extends _{static litTagName=s.literal`devtools-performance-third-parties`;insightCategory=L.ALL;internalName="third-parties";#Z=new Map;createOverlays(){if(this.#Z.clear(),!this.model)return[];const e=[];for(const[t,s]of this.model.requestsByEntity){if(t===this.model.firstPartyEntity)continue;const i=[];for(const t of s){const s={type:"ENTRY_OUTLINE",entry:t,outlineReason:"INFO"};i.push(s),e.push(s)}this.#Z.set(t,i)}return e}#s(t){if(!this.model)return s.nothing;const n=t.sort(((e,t)=>t[1].transferSize-e[1].transferSize)).slice(0,6),o=t.sort(((e,t)=>t[1].mainThreadTime-e[1].mainThreadTime)).slice(0,6);return ze`
        <div class="insights">
            <devtools-performance-sidebar-insight .data=${{title:this.model.title,description:this.model.description,internalName:this.internalName,expanded:this.isActive()}}
            @insighttoggleclick=${this.onSidebarClick}>
                <div slot="insight-content">
                  <div class="insight-section">
                    ${ze`<devtools-performance-table
                      .data=${{insight:this,headers:[Fe(Oe.columnThirdParty),Fe(Oe.columnTransferSize)],rows:n.map((([t,s])=>({values:[t.name,e.ByteUtilities.bytesToString(s.transferSize)],overlays:this.#Z.get(t)})))}}>
                    </devtools-performance-table>`}
                  </div>

                  <div class="insight-section">
                    ${ze`<devtools-performance-table
                      .data=${{insight:this,headers:[Fe(Oe.columnThirdParty),Fe(Oe.columnBlockingTime)],rows:o.map((([t,s])=>({values:[t.name,e.TimeUtilities.millisToString(i.Timing.microSecondsToMilliSeconds(s.mainThreadTime))],overlays:this.#Z.get(t)})))}}>
                    </devtools-performance-table>`}
                  </div>
                </div>
            </devtools-performance-sidebar-insight>
        </div>`}render(){const e=this.model,t=e&&[...e.summaryByEntity.entries()].filter((t=>t[0]!==e.firstPartyEntity)),i=t?.length,n=D({activeCategory:this.data.activeCategory,insightCategory:this.insightCategory}),o=i&&n?this.#s(t):s.nothing;s.render(o,this.shadow,{host:this})}}customElements.define("devtools-performance-third-parties",qe);var Be=Object.freeze({__proto__:null,ThirdParties:qe});const{html:je}=s;class Ue extends _{static litTagName=s.literal`devtools-performance-viewport`;insightCategory=L.INP;internalName="viewport";createOverlays(){return[]}#s(e){if(!this.model)return s.nothing;const t=e.viewportEvent?.args.data.node_id;return je`
        <div class="insights">
            <devtools-performance-sidebar-insight .data=${{title:this.model.title,description:this.model.description,expanded:this.isActive(),internalName:this.internalName,estimatedSavingsTime:e.metricSavings?.INP}}
            @insighttoggleclick=${this.onSidebarClick}>
              ${void 0!==t?je`<devtools-performance-node-link
                .data=${{backendNodeId:t,options:{tooltip:e.viewportEvent?.args.data.content}}}>
              </devtools-performance-node-link>`:s.nothing}
            </devtools-performance-sidebar-insight>
        </div>`}render(){const e=this.model,t=e&&!1===e.mobileOptimized,i=D({activeCategory:this.data.activeCategory,insightCategory:this.insightCategory}),n=t&&i?this.#s(e):s.nothing;s.render(n,this.shadow,{host:this})}}customElements.define("devtools-performance-viewport",Ue);var We=Object.freeze({__proto__:null,Viewport:Ue});export{F as CLSCulprits,K as DocumentLatency,v as EventRef,se as FontDisplay,P as Helpers,le as InteractionToNextPaint,ve as LCPDiscovery,Te as LCPPhases,Ce as NodeLink,Ie as RenderBlocking,N as SidebarInsight,Ae as SlowCSSSelector,J as Table,Be as ThirdParties,$ as Types,We as Viewport};
