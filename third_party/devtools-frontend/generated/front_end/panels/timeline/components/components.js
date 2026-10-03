import*as e from"../../../services/trace_bounds/trace_bounds.js";import*as t from"../../../core/i18n/i18n.js";import*as i from"../../../models/trace/trace.js";import*as n from"../../../ui/components/helpers/helpers.js";import*as o from"../../../ui/legacy/legacy.js";import*as r from"../../../ui/lit-html/lit-html.js";import*as s from"../../../ui/visual_logging/visual_logging.js";import"../../../ui/components/menus/menus.js";import*as a from"../../../core/sdk/sdk.js";import*as l from"../../mobile_throttling/mobile_throttling.js";import*as d from"../../../core/platform/platform.js";import"../../../ui/components/data_grid/data_grid.js";import*as c from"../../../models/crux-manager/crux-manager.js";import"../../../ui/components/buttons/buttons.js";import"../../../ui/components/dialogs/dialogs.js";import*as h from"../../../ui/components/input/input.js";import*as u from"../../../models/trace/helpers/helpers.js";import*as g from"../../../ui/legacy/components/utils/utils.js";import*as p from"../utils/utils.js";import*as m from"./insights/insights.js";import"../../../ui/components/icon_button/icon_button.js";import*as v from"../../../core/common/common.js";import*as b from"../../../core/host/host.js";import*as f from"../../../ui/legacy/theme_support/theme_support.js";import*as y from"../../../models/emulation/emulation.js";import*as w from"../../../models/live-metrics/live-metrics.js";import*as S from"../../../ui/components/legacy_wrapper/legacy_wrapper.js";import*as k from"../../../ui/components/render_coordinator/render_coordinator.js";import"../../../ui/components/request_link_icon/request_link_icon.js";import*as x from"../../../ui/legacy/components/perf_ui/perf_ui.js";import*as C from"../../../core/root/root.js";import*as P from"../../../ui/components/adorners/adorners.js";function $(e){const t=[e];let i=e;for(;null!==i.child;){const e=i.child;null!==e&&(t.push(e),i=e)}return t}var T=Object.freeze({__proto__:null,Breadcrumbs:class{initialBreadcrumb;activeBreadcrumb;constructor(e){this.initialBreadcrumb={window:e,child:null};let t=this.initialBreadcrumb;for(;null!==t.child;)t=t.child;this.activeBreadcrumb=t}add(e){if(!this.isTraceWindowWithinTraceWindow(e,this.activeBreadcrumb.window))throw new Error("Can not add a breadcrumb that is equal to or is outside of the parent breadcrumb TimeWindow");const t={window:e,child:null};return this.activeBreadcrumb.child=t,this.setActiveBreadcrumb(t,{removeChildBreadcrumbs:!1,updateVisibleWindow:!0}),t}isTraceWindowWithinTraceWindow(e,t){return e.min>=t.min&&e.max<=t.max&&!(e.min===t.min&&e.max===t.max)}setInitialBreadcrumbFromLoadedModifications(e){this.initialBreadcrumb=e;let t=e;for(;null!==t.child;)t=t.child;this.setActiveBreadcrumb(t,{removeChildBreadcrumbs:!1,updateVisibleWindow:!0})}setActiveBreadcrumb(t,i){i.removeChildBreadcrumbs&&(t.child=null),this.activeBreadcrumb=t,e.TraceBounds.BoundsManager.instance().setMiniMapBounds(t.window),i.updateVisibleWindow&&e.TraceBounds.BoundsManager.instance().setTimelineVisibleWindow(t.window)}},flattenBreadcrumbs:$});const L=new CSSStyleSheet;L.replaceSync(".breadcrumbs{display:none;align-items:center;height:29px;padding:3px;overflow-y:hidden;overflow-x:scroll}.breadcrumbs::-webkit-scrollbar{display:none}.breadcrumb{padding:2px 6px;border-radius:4px}.breadcrumb:hover{background-color:var(--sys-color-state-hover-on-subtle)}.range{font-size:12px;white-space:nowrap}.active-breadcrumb{font-weight:bold;color:var(--app-color-active-breadcrumb)}\n/*# sourceURL=breadcrumbsUI.css */\n");const{render:R,html:I}=r,E={activateBreadcrumb:"Activate breadcrumb",removeChildBreadcrumbs:"Remove child breadcrumbs"},M=t.i18n.registerUIStrings("panels/timeline/components/BreadcrumbsUI.ts",E),D=t.i18n.getLocalizedString.bind(void 0,M);class H extends Event{breadcrumb;childBreadcrumbsRemoved;static eventName="breadcrumbactivated";constructor(e,t){super(H.eventName),this.breadcrumb=e,this.childBreadcrumbsRemoved=t}}class F extends HTMLElement{#e=this.attachShadow({mode:"open"});#t=this.#i.bind(this);#n=null;#o=null;connectedCallback(){this.#e.adoptedStyleSheets=[L]}set data(e){this.#n=e.initialBreadcrumb,this.#o=e.activeBreadcrumb,n.ScheduledRender.scheduleRender(this,this.#t)}#r(e){this.#o=e,this.dispatchEvent(new H(e))}#s(){const e=this.#e.querySelector(".breadcrumbs");e&&(e.style.display="flex",requestAnimationFrame((()=>{e.scrollWidth-e.clientWidth>0&&requestAnimationFrame((()=>{e.scrollLeft=e.scrollWidth-e.clientWidth}))})))}#a(e,t){const i=new o.ContextMenu.ContextMenu(e);i.defaultSection().appendItem(D(E.activateBreadcrumb),(()=>{this.dispatchEvent(new H(t))})),i.defaultSection().appendItem(D(E.removeChildBreadcrumbs),(()=>{this.dispatchEvent(new H(t,!0))})),i.show()}#l(e,n){const o=i.Helpers.Timing.microSecondsToMilliseconds(e.window.range);return I`
          <div class="breadcrumb" @contextmenu=${t=>this.#a(t,e)} @click=${()=>this.#r(e)}
          jslog=${s.item("timeline.breadcrumb-select").track({click:!0})}>
           <span class="${e===this.#o?"active-breadcrumb":""} range">
            ${0===n?`Full range (${t.TimeUtilities.preciseMillisToString(o,2)})`:`${t.TimeUtilities.preciseMillisToString(o,2)}`}
            </span>
          </div>
          ${null!==e.child?I`
            <devtools-icon .data=${{iconName:"chevron-right",color:"var(--icon-default)",width:"16px",height:"16px"}}>`:""}
      `}#i(){const e=I`
      ${null===this.#n?r.nothing:I`<div class="breadcrumbs" jslog=${s.section("breadcrumbs")}>
        ${$(this.#n).map(((e,t)=>this.#l(e,t)))}
      </div>`}
    `;R(e,this.#e,{host:this}),this.#n?.child&&this.#s()}}customElements.define("devtools-breadcrumbs-ui",F);var O=Object.freeze({__proto__:null,BreadcrumbActivatedEvent:H,BreadcrumbsUI:F});const N=new CSSStyleSheet;N.replaceSync(":host{display:inline-block;max-width:100%;min-width:50px}devtools-select-menu{max-width:100%}\n/*# sourceURL=cpuThrottlingSelector.css */\n");const{html:A}=r,z={cpu:"CPU: {PH1}",cpuThrottling:"CPU throttling: {PH1}",noThrottling:"No throttling",dSlowdown:"{PH1}× slowdown"},U=t.i18n.registerUIStrings("panels/timeline/components/CPUThrottlingSelector.ts",z),_=t.i18n.getLocalizedString.bind(void 0,U);class B extends HTMLElement{#e=this.attachShadow({mode:"open"});#d;constructor(){super(),this.#d=a.CPUThrottlingManager.CPUThrottlingManager.instance().cpuThrottlingRate(),this.#i()}connectedCallback(){this.#e.adoptedStyleSheets=[N],a.CPUThrottlingManager.CPUThrottlingManager.instance().addEventListener("RateChanged",this.#c,this),this.#c()}disconnectedCallback(){a.CPUThrottlingManager.CPUThrottlingManager.instance().removeEventListener("RateChanged",this.#c,this)}#c(){this.#d=a.CPUThrottlingManager.CPUThrottlingManager.instance().cpuThrottlingRate(),n.ScheduledRender.scheduleRender(this,this.#i)}#h(e){l.ThrottlingManager.throttlingManager().setCPUThrottlingRate(Number(e.itemValue))}#i=()=>{const e=1===this.#d?_(z.noThrottling):_(z.dSlowdown,{PH1:this.#d}),t=A`
      <devtools-select-menu
            @selectmenuselected=${this.#h}
            .showDivider=${!0}
            .showArrow=${!0}
            .sideButton=${!1}
            .showSelectedItem=${!0}
            .showConnector=${!1}
            .jslogContext=${"cpu-throttling"}
            .buttonTitle=${_(z.cpu,{PH1:e})}
            title=${_(z.cpuThrottling,{PH1:e})}
          >
          ${l.ThrottlingPresets.ThrottlingPresets.cpuThrottlingPresets.map((e=>{const t=1===e?_(z.noThrottling):_(z.dSlowdown,{PH1:e}),i=1===e?"cpu-no-throttling":`cpu-throttled-${e}`;return A`
              <devtools-menu-item
                .value=${e}
                .selected=${this.#d===e}
                jslog=${s.item(i).track({click:!0})}
              >
                ${t}
              </devtools-menu-item>
            `}))}
      </devtools-select-menu>
    `;r.render(t,this.#e,{host:this})}}customElements.define("devtools-cpu-throttling-selector",B);var q=Object.freeze({__proto__:null,CPUThrottlingSelector:B});const V={forcedReflow:"Forced reflow",sIsALikelyPerformanceBottleneck:"{PH1} is a likely performance bottleneck.",idleCallbackExecutionExtended:"Idle callback execution extended beyond deadline by {PH1}",sTookS:"{PH1} took {PH2}.",longTask:"Long task",longInteractionINP:"Long interaction",sIsLikelyPoorPageResponsiveness:"{PH1} is indicating poor page responsiveness.",websocketProtocol:"WebSocket protocol",webSocketBytes:"{PH1} byte(s)",webSocketDataLength:"Data length"},W=t.i18n.registerUIStrings("panels/timeline/components/DetailsView.ts",V),j=t.i18n.getLocalizedString.bind(void 0,W);var G=Object.freeze({__proto__:null,buildRowsForWebSocketEvent:function(e,n){const o=[],r=n.Initiators.eventToInitiator.get(e);return r&&i.Types.Events.isWebSocketCreate(r)?(o.push({key:t.i18n.lockedString("URL"),value:r.args.data.url}),r.args.data.websocketProtocol&&o.push({key:j(V.websocketProtocol),value:r.args.data.websocketProtocol})):i.Types.Events.isWebSocketCreate(e)&&(o.push({key:t.i18n.lockedString("URL"),value:e.args.data.url}),e.args.data.websocketProtocol&&o.push({key:j(V.websocketProtocol),value:e.args.data.websocketProtocol})),i.Types.Events.isWebSocketTransfer(e)&&e.args.data.dataLength&&o.push({key:j(V.webSocketDataLength),value:`${j(V.webSocketBytes,{PH1:e.args.data.dataLength})}`}),o},buildWarningElementsForEvent:function(e,n){const r=n.Warnings.perEvent.get(e),s=[];if(!r)return s;for(const n of r){const r=i.Helpers.Timing.microSecondsToMilliseconds(i.Types.Timing.MicroSeconds(e.dur||0)),a=document.createElement("span");switch(n){case"FORCED_REFLOW":{const e=o.XLink.XLink.create("https://developers.google.com/web/fundamentals/performance/rendering/avoid-large-complex-layouts-and-layout-thrashing#avoid-forced-synchronous-layouts",j(V.forcedReflow),void 0,void 0,"forced-reflow");a.appendChild(t.i18n.getFormatLocalizedString(W,V.sIsALikelyPerformanceBottleneck,{PH1:e}));break}case"IDLE_CALLBACK_OVER_TIME":{if(!i.Types.Events.isFireIdleCallback(e))break;const n=t.TimeUtilities.millisToString((r||0)-e.args.data.allottedMilliseconds,!0);a.textContent=j(V.idleCallbackExecutionExtended,{PH1:n});break}case"LONG_TASK":{const e=o.XLink.XLink.create("https://web.dev/optimize-long-tasks/",j(V.longTask),void 0,void 0,"long-tasks");a.appendChild(t.i18n.getFormatLocalizedString(W,V.sTookS,{PH1:e,PH2:t.TimeUtilities.millisToString(r||0,!0)}));break}case"LONG_INTERACTION":{const e=o.XLink.XLink.create("https://web.dev/inp",j(V.longInteractionINP),void 0,void 0,"long-interaction");a.appendChild(t.i18n.getFormatLocalizedString(W,V.sIsLikelyPoorPageResponsiveness,{PH1:e}));break}default:d.assertNever(n,`Unhandled warning type ${n}`)}s.push(a)}return s},generateInvalidationsList:function(e){const t={},n=new Set;for(const o of e){n.add(o.args.data.nodeId);let e=o.args.data.reason||"unknown";if("unknown"===e&&i.Types.Events.isScheduleStyleInvalidationTracking(o)&&o.args.data.invalidatedSelectorId)switch(o.args.data.invalidatedSelectorId){case"attribute":e="Attribute",o.args.data.changedAttribute&&(e+=` (${o.args.data.changedAttribute})`);break;case"class":e="Class",o.args.data.changedClass&&(e+=` (${o.args.data.changedClass})`);break;case"id":e="Id",o.args.data.changedId&&(e+=` (${o.args.data.changedId})`)}if("PseudoClass"===e&&i.Types.Events.isStyleRecalcInvalidationTracking(o)&&o.args.data.extraData&&(e+=o.args.data.extraData),"Attribute"===e&&i.Types.Events.isStyleRecalcInvalidationTracking(o)&&o.args.data.extraData&&(e+=` (${o.args.data.extraData})`),"StyleInvalidator"===e)continue;const r=t[e]||[];r.push(o),t[e]=r}return{groupedByReason:t,backendNodeIds:n}}});const K=new CSSStyleSheet;K.replaceSync(':host{display:block}:host *{box-sizing:border-box}devtools-dialog{--override-transparent:color-mix(in sRGB,var(--color-background) 80%,transparent)}.title{font-size:var(--sys-typescale-headline4-size);line-height:var(--sys-typescale-headline4-line-height);font-weight:var(--ref-typeface-weight-medium);margin:0}.section-title{font-size:var(--sys-typescale-headline5-size);line-height:var(--sys-typescale-headline5-line-height);font-weight:var(--ref-typeface-weight-medium);margin:0}.privacy-disclosure{margin:8px 0}.url-override{margin:8px 0;display:flex;align-items:center;overflow:hidden;text-overflow:ellipsis;max-width:max-content}details > summary{font-size:var(--sys-typescale-body4-size);line-height:var(--sys-typescale-body4-line-height);font-weight:var(--ref-typeface-weight-medium)}.content{max-width:360px;padding:16px 20px 18px;box-sizing:border-box}.open-button-section{display:flex;flex-direction:row}.origin-mapping-grid{border:1px solid var(--sys-color-divider);margin-top:8px}.origin-mapping-button-section{display:flex;flex-direction:column;align-items:center;margin-top:6px}.config-button{margin-left:auto}.advanced-section-contents{margin:4px 0 14px}.buttons-section{display:flex;justify-content:flex-end;margin-top:6px;gap:8px}input[type="checkbox"]{height:12px;width:12px;min-height:12px;min-width:12px;margin:6px}input[type="text"][disabled]{color:var(--sys-color-state-disabled)}.warning{margin:2px 8px;color:var(--color-error-text)}x-link{color:var(--sys-color-primary);text-decoration-line:underline}.divider{margin:10px 0;border:none;border-top:1px solid var(--sys-color-divider)}\n/*# sourceURL=fieldSettingsDialog.css */\n');const Y={setUp:"Set up",configure:"Configure",ok:"Ok",optOut:"Opt out",cancel:"Cancel",onlyFetchFieldData:"Always show field data for the below URL",url:"URL",doesNotHaveSufficientData:"The Chrome UX Report does not have sufficient real-world speed data for this page.",configureFieldData:"Configure field data fetching",fetchAggregated:"Fetch aggregated field data from the {PH1} to help you contextualize local measurements with what real users experience on the site.",privacyDisclosure:"Privacy disclosure",whenPerformanceIsShown:"When DevTools is open, the URLs you visit will be sent to Google to query field data. These requests are not tied to your Google account.",advanced:"Advanced",mapDevelopmentOrigins:"Set a development origin to automatically get relevant field data for its production origin.",developmentOrigin:"Development origin",productionOrigin:"Production origin",developmentOriginValue:"Development origin: {PH1}",productionOriginValue:"Production origin: {PH1}",new:"New",add:"Add",delete:"Delete",invalidOrigin:'"{PH1}" is not a valid origin or URL.',alreadyMapped:'"{PH1}" is already mapped to a production origin.'},X=t.i18n.registerUIStrings("panels/timeline/components/FieldSettingsDialog.ts",Y),J=t.i18n.getLocalizedString.bind(void 0,X),{html:Z,nothing:Q,Directives:{ifDefined:ee}}=r;class te extends Event{static eventName="showdialog";constructor(){super(te.eventName)}}class ie extends HTMLElement{#e=this.attachShadow({mode:"open"});#u;#g=c.CrUXManager.instance().getConfigSetting();#p="";#m=!1;#v="";#b="";#f=[];#y=!1;#w="";#S="";constructor(){super();const e=c.CrUXManager.instance();this.#g=e.getConfigSetting(),this.#k(),this.#i()}#k(){const e=this.#g.get();this.#p=e.override||"",this.#m=e.overrideEnabled||!1,this.#f=e.originMappings||[],this.#v="",this.#b="",this.#y=!1,this.#w="",this.#S=""}#x(e){this.#g.set({enabled:e,override:this.#p,originMappings:this.#f,overrideEnabled:this.#m})}#C(){n.ScheduledRender.scheduleRender(this,this.#i)}async#P(e){const t=c.CrUXManager.instance(),i=await t.getFieldDataForPage(e);return Object.values(i).some((e=>e))}async#$(e){if(e&&this.#m){if(!this.#T(this.#p))return this.#v=J(Y.invalidOrigin,{PH1:this.#p}),void n.ScheduledRender.scheduleRender(this,this.#i);if(!await this.#P(this.#p))return this.#v=J(Y.doesNotHaveSufficientData),void n.ScheduledRender.scheduleRender(this,this.#i)}this.#x(e),this.#L()}#R(){if(!this.#u)throw new Error("Dialog not found");this.#k(),this.#u.setDialogVisible(!0),n.ScheduledRender.scheduleRender(this,this.#i),this.dispatchEvent(new te)}#L(e){if(!this.#u)throw new Error("Dialog not found");this.#u.setDialogVisible(!1),e&&e.stopImmediatePropagation(),n.ScheduledRender.scheduleRender(this,this.#i)}connectedCallback(){this.#e.adoptedStyleSheets=[K,h.textInputStyles,h.checkboxStyles],this.#g.addChangeListener(this.#C,this),n.ScheduledRender.scheduleRender(this,this.#i)}disconnectedCallback(){this.#g.removeChangeListener(this.#C,this)}#I(){return this.#g.get().enabled?Z`
        <devtools-button
          class="config-button"
          @click=${this.#R}
          .data=${{variant:"outlined",title:J(Y.configure)}}
        jslog=${s.action("timeline.field-data.configure").track({click:!0})}
        >${J(Y.configure)}</devtools-button>
      `:Z`
      <devtools-button
        class="setup-button"
        @click=${this.#R}
        .data=${{variant:"primary",title:J(Y.setUp)}}
        jslog=${s.action("timeline.field-data.setup").track({click:!0})}
        data-field-data-setup
      >${J(Y.setUp)}</devtools-button>
    `}#E(){return Z`
      <devtools-button
        @click=${()=>{this.#$(!0)}}
        .data=${{variant:"primary",title:J(Y.ok)}}
        jslog=${s.action("timeline.field-data.enable").track({click:!0})}
        data-field-data-enable
      >${J(Y.ok)}</devtools-button>
    `}#M(){const e=this.#g.get().enabled?J(Y.optOut):J(Y.cancel);return Z`
      <devtools-button
        @click=${()=>{this.#$(!1)}}
        .data=${{variant:"outlined",title:e}}
        jslog=${s.action("timeline.field-data.disable").track({click:!0})}
        data-field-data-disable
      >${e}</devtools-button>
    `}#D(e){e.stopPropagation();const t=e.target;this.#p=t.value,this.#v="",n.ScheduledRender.scheduleRender(this,this.#i)}#H(e){e.stopPropagation();const t=e.target;this.#m=t.checked,this.#v="",n.ScheduledRender.scheduleRender(this,this.#i)}#F=e=>{e.stopPropagation();const t=e.target;this.#w=t.value,n.ScheduledRender.scheduleRender(this,this.#i)};#O=e=>{e.stopPropagation();const t=e.target;this.#S=t.value,n.ScheduledRender.scheduleRender(this,this.#i)};#T(e){try{return new URL(e).origin}catch{return null}}#N(){this.#w="",this.#S="",this.#y=!0,this.#b="",n.ScheduledRender.scheduleRender(this,this.#i)}async#A(){const e=this.#T(this.#w),t=this.#T(this.#S);if(!e)return this.#b=J(Y.invalidOrigin,{PH1:this.#w}),void n.ScheduledRender.scheduleRender(this,this.#i);if(this.#f.find((t=>t.developmentOrigin===e)))return this.#b=J(Y.alreadyMapped,{PH1:e}),void n.ScheduledRender.scheduleRender(this,this.#i);if(!t)return this.#b=J(Y.invalidOrigin,{PH1:this.#S}),void n.ScheduledRender.scheduleRender(this,this.#i);if(!await this.#P(t))return this.#b=J(Y.doesNotHaveSufficientData,{PH1:this.#S}),void n.ScheduledRender.scheduleRender(this,this.#i);this.#f.push({developmentOrigin:e,productionOrigin:t}),this.#w="",this.#S="",this.#y=!1,this.#b="",n.ScheduledRender.scheduleRender(this,this.#i)}#z(e){this.#f.splice(e,1),n.ScheduledRender.scheduleRender(this,this.#i)}#U(){const e=this.#f.map(((e,t)=>({cells:[{columnId:"development-origin",value:e.developmentOrigin,title:e.developmentOrigin},{columnId:"production-origin",value:e.productionOrigin,title:e.productionOrigin},{columnId:"action-button",value:J(Y.delete),renderer:e=>Z`
              <div style="display: flex; align-items: center; justify-content: center;">
                <devtools-button
                  class="delete-mapping"
                  .data=${{variant:"icon",size:"SMALL",title:e,iconName:"bin",jslogContext:"delete-origin-mapping"}}
                  @click=${()=>this.#z(t)}
                ></devtools-button>
              </div>
            `}]})));if(this.#y){const t="width: 100%; box-sizing: border-box; border: none; background: none;";e.push({cells:[{columnId:"development-origin",value:this.#w,renderer:e=>Z`
              <input
                type="text"
                placeholder="http://localhost:8080"
                aria-label=${J(Y.developmentOriginValue,{PH1:e})}
                style=${t}
                title=${ee(e)}
                @keyup=${this.#F}
                @change=${this.#F} />
            `},{columnId:"production-origin",value:this.#S,renderer:e=>Z`
              <input
                type="text"
                placeholder="https://example.com"
                aria-label=${J(Y.productionOriginValue,{PH1:e})}
                style=${t}
                title=${ee(e)}
                @keyup=${this.#O}
                @change=${this.#O} />
            `},{columnId:"action-button",value:J(Y.add),renderer:e=>Z`
              <div style="display: flex; align-items: center; justify-content: center;">
                <devtools-button
                  id="add-mapping-button"
                  .data=${{variant:"icon",size:"SMALL",title:e,iconName:"plus",disabled:!this.#w||!this.#S,jslogContext:"add-origin-mapping"}}
                  @click=${()=>this.#A()}
                ></devtools-button>
              </div>
            `}]})}const t={columns:[{id:"development-origin",title:J(Y.developmentOrigin),widthWeighting:13,hideable:!1,visible:!0,sortable:!1},{id:"production-origin",title:J(Y.productionOrigin),widthWeighting:13,hideable:!1,visible:!0,sortable:!1},{id:"action-button",title:"",widthWeighting:3,hideable:!1,visible:!0,sortable:!1}],rows:e};return Z`
      <div>${J(Y.mapDevelopmentOrigins)}</div>
      <devtools-data-grid-controller
        class="origin-mapping-grid"
        .data=${t}
      ></devtools-data-grid-controller>
      ${this.#b?Z`
        <div class="warning" role="alert" aria-label=${this.#b}>${this.#b}</div>
      `:Q}
      <div class="origin-mapping-button-section">
        <devtools-button
          @click=${this.#N}
          .data=${{variant:"text",title:J(Y.new),iconName:"plus",disabled:this.#y}}
          jslogContext=${"new-origin-mapping"}
        >${J(Y.new)}</devtools-button>
      </div>
    `}#i=()=>{const e=o.XLink.XLink.create("https://developer.chrome.com/docs/crux",t.i18n.lockedString("Chrome UX Report")),i=t.i18n.getFormatLocalizedString(X,Y.fetchAggregated,{PH1:e}),a=Z`
      <div class="open-button-section">${this.#I()}</div>
      <devtools-dialog
        @clickoutsidedialog=${this.#L}
        .showConnector=${!0}
        .position=${"auto"}
        .horizontalAlignment=${"center"}
        .jslogContext=${"timeline.field-data.settings"}
        on-render=${n.Directives.nodeRenderedCallback((e=>{this.#u=e}))}
      >
        <div class="content">
          <h2 class="title">${J(Y.configureFieldData)}</h2>
          <div>${i}</div>
          <div class="privacy-disclosure">
            <h3 class="section-title">${J(Y.privacyDisclosure)}</h3>
            <div>${J(Y.whenPerformanceIsShown)}</div>
          </div>
          <details aria-label=${J(Y.advanced)}>
            <summary>${J(Y.advanced)}</summary>
            <div class="advanced-section-contents">
              ${this.#U()}
              <hr class="divider">
              <label class="url-override">
                <input
                  type="checkbox"
                  .checked=${this.#m}
                  @change=${this.#H}
                  aria-label=${J(Y.onlyFetchFieldData)}
                  jslog=${s.toggle().track({click:!0}).context("field-url-override-enabled")}
                />
                ${J(Y.onlyFetchFieldData)}
              </label>
              <input
                type="text"
                @keyup=${this.#D}
                @change=${this.#D}
                class="devtools-text-input"
                .disabled=${!this.#m}
                .value=${this.#p}
                placeholder=${ee(this.#m?J(Y.url):void 0)}
              />
              ${this.#v?Z`<div class="warning" role="alert" aria-label=${this.#v}>${this.#v}</div>`:Q}
            </div>
          </details>
          <div class="buttons-section">
            ${this.#M()}
            ${this.#E()}
          </div>
        </div>
      </devtools-dialog>
    `;r.render(a,this.#e,{host:this})}}customElements.define("devtools-field-settings-dialog",ie);var ne=Object.freeze({__proto__:null,FieldSettingsDialog:ie,ShowDialog:te});const oe=new CSSStyleSheet;oe.replaceSync(":host{display:block}.breakdown{margin:0;padding:0;list-style:none;color:var(--sys-color-token-subtle)}.value{display:inline-block;padding:0 5px;color:var(--sys-color-on-surface)}\n/*# sourceURL=interactionBreakdown.css */\n");const{html:re}=r,se={inputDelay:"Input delay",processingDuration:"Processing duration",presentationDelay:"Presentation delay"},ae=t.i18n.registerUIStrings("panels/timeline/components/InteractionBreakdown.ts",se),le=t.i18n.getLocalizedString.bind(void 0,ae);class de extends HTMLElement{#e=this.attachShadow({mode:"open"});#t=this.#i.bind(this);#_=null;connectedCallback(){this.#e.adoptedStyleSheets=[oe]}set entry(e){e!==this.#_&&(this.#_=e,n.ScheduledRender.scheduleRender(this,this.#t))}#i(){if(!this.#_)return;const e=t.TimeUtilities.formatMicroSecondsAsMillisFixed(this.#_.inputDelay),i=t.TimeUtilities.formatMicroSecondsAsMillisFixed(this.#_.mainThreadHandling),n=t.TimeUtilities.formatMicroSecondsAsMillisFixed(this.#_.presentationDelay);r.render(re`<ul class="breakdown">
                     <li data-entry="input-delay">${le(se.inputDelay)}<span class="value">${e}</span></li>
                     <li data-entry="processing-duration">${le(se.processingDuration)}<span class="value">${i}</span></li>
                     <li data-entry="presentation-delay">${le(se.presentationDelay)}<span class="value">${n}</span></li>
                   </ul>
                   `,this.#e,{host:this})}}customElements.define("devtools-interaction-breakdown",de);var ce=Object.freeze({__proto__:null,InteractionBreakdown:de});const he=new CSSStyleSheet;he.replaceSync(".layout-shift-details-title,\n.cluster-details-title{padding-bottom:var(--sys-size-5);display:flex;align-items:center;.layout-shift-event-title,\n  .cluster-event-title{background-color:var(--app-color-rendering);width:var(--sys-size-6);height:var(--sys-size-6);border:var(--sys-size-1) solid var(--sys-color-divider);display:inline-block;margin-right:var(--sys-size-3)}}.layout-shift-details-table{font:var(--sys-typescale-body4-regular);margin-bottom:var(--sys-size-4);text-align:left;border-block:var(--sys-size-1) solid var(--sys-color-divider);border-collapse:collapse;font-variant-numeric:tabular-nums;th,\n  td{padding-right:var(--sys-size-4);min-width:var(--sys-size-20);max-width:var(--sys-size-28)}}.table-title{th{font:var(--sys-typescale-body4-medium)}tr{border-bottom:var(--sys-size-1) solid var(--sys-color-divider)}}.timeline-link{cursor:pointer;text-decoration:underline;color:var(--sys-color-primary);background:none;border:none;padding:0;font:inherit}.timeline-link.invalid-link{color:var(--sys-color-state-disabled)}.details-row{display:flex;min-height:var(--sys-size-9)}.title{color:var(--sys-color-token-subtle);overflow:hidden;padding-right:var(--sys-size-5);display:inline-block;vertical-align:top}.culprit{display:inline-flex;flex-direction:row;gap:var(--sys-size-3)}.value{display:inline-block;user-select:text;text-overflow:ellipsis;overflow:hidden;padding:0 var(--sys-size-3)}.layout-shift-summary-details,\n.layout-shift-cluster-summary-details{font:var(--sys-typescale-body4-regular);display:flex;flex-direction:column;column-gap:var(--sys-size-4);padding:var(--sys-size-6) var(--sys-size-6) 0 var(--sys-size-6)}.culprits{display:flex;flex-direction:column}.shift-row:not(:last-child){border-bottom:var(--sys-size-1) solid var(--sys-color-divider)}.total-row{font:var(--sys-typescale-body4-medium)}\n/*# sourceURL=layoutShiftDetails.css */\n");const{html:ue}=r,ge={startTime:"Start time",shiftScore:"Shift score",elementsShifted:"Elements shifted",culprit:"Culprit",injectedIframe:"Injected iframe",fontRequest:"Font request",nonCompositedAnimation:"Non-composited animation",animation:"Animation",parentCluster:"Parent cluster",cluster:"Layout shift cluster @ {PH1}",layoutShift:"Layout shift @ {PH1}",total:"Total",unsizedImage:"Unsized image"},pe=t.i18n.registerUIStrings("panels/timeline/components/LayoutShiftDetails.ts",ge),me=t.i18n.getLocalizedString.bind(void 0,pe);class ve extends HTMLElement{#e=this.attachShadow({mode:"open"});#B=null;#q=null;#V=null;#W=!1;connectedCallback(){this.#e.adoptedStyleSheets=[he],o.UIUtils.injectTextButtonStyles(this.#e),this.#i()}setData(e,t,i,n){this.#B!==e&&(this.#B=e,this.#q=t,this.#V=i,this.#W=n,this.#i())}#j(e){const t=p.EntryName.nameForEntry(e);return ue`
      <div class="layout-shift-details-title">
        <div class="layout-shift-event-title"></div>
        ${t}
      </div>
    `}#G(e){return ue`
      ${e?.map((e=>void 0!==e.node_id?ue`
            <devtools-performance-node-link .data=${{backendNodeId:e.node_id}}>
            </devtools-performance-node-link>`:r.nothing))}`}#K(e){const t=e;if(!t)return null;const i=a.FrameManager.FrameManager.instance().getFrame(t);if(!i)return null;const n=g.Linkifier.Linkifier.linkifyRevealable(i,i.displayName());return ue`
    <span class="culprit"><span class="culprit-type">${me(ge.injectedIframe)}: </span><span class="culprit-value">${n}</span></span>`}#Y(e){const t={tabStop:!0,showColumnNumber:!1,inlineFrameIndex:0,maxLength:20},i=g.Linkifier.Linkifier.linkifyURL(e.args.data.url,t);return ue`
    <span class="culprit"><span class="culprit-type">${me(ge.fontRequest)}: </span><span class="culprit-value">${i}</span></span>`}#X(e){this.dispatchEvent(new m.EventRef.EventReferenceClick(e))}#J(e){const t=e.animation;return t?ue`
        <span class="culprit">
        <span class="culprit-type">${me(ge.nonCompositedAnimation)}: </span>
        <button type="button" class="culprit-value timeline-link" @click=${()=>this.#X(t)}>${me(ge.animation)}</button>
      </span>`:null}#Z(e){const t=ue`
      <devtools-performance-node-link
        .data=${{backendNodeId:e}}>
      </devtools-performance-node-link>`;return ue`
    <span class="culprit"><span class="culprit-type">${me(ge.unsizedImage)}: </span><span class="culprit-value">${t}</span></span>`}#Q(e){return ue`
      ${e?.fontRequests.map((e=>this.#Y(e)))}
      ${e?.iframeIds.map((e=>this.#K(e)))}
      ${e?.nonCompositedAnimations.map((e=>this.#J(e)))}
      ${e?.unsizedImages.map((e=>this.#Z(e)))}
    `}#ee(e,n){const o=i.Types.Timing.MicroSeconds(e.ts-n.Meta.traceBounds.min);if(e===this.#B)return ue`${t.TimeUtilities.preciseMillisToString(u.Timing.microSecondsToMilliseconds(o))}`;const r=t.TimeUtilities.formatMicroSecondsTime(o);return ue`
         <button type="button" class="timeline-link" @click=${()=>this.#X(e)}>${me(ge.layoutShift,{PH1:r})}</button>`}#te(e,t,i,n){const o=e.args.data?.weighted_score_delta;if(!o)return null;const s=Boolean(n&&(n.fontRequests.length||n.iframeIds.length||n.nonCompositedAnimations.length||n.unsizedImages.length));return ue`
      <tr class="shift-row" data-ts=${e.ts}>
        <td>${this.#ee(e,t)}</td>
        <td>${o.toFixed(4)}</td>
        ${this.#W?ue`
          <td>
            <div class="elements-shifted">
              ${this.#G(i)}
            </div>
          </td>`:r.nothing}
        ${s&&this.#W?ue`
          <td class="culprits">
            ${this.#Q(n)}
          </td>`:r.nothing}
      </tr>`}#ie(e,n){if(!e)return null;const o=i.Types.Timing.MicroSeconds(e.ts-(n?.Meta.traceBounds.min??0)),r=t.TimeUtilities.formatMicroSecondsTime(o);return ue`
      <span class="parent-cluster">${me(ge.parentCluster)}:
         <button type="button" class="timeline-link" @click=${()=>this.#X(e)}>${me(ge.cluster,{PH1:r})}</button>
      </span>`}#ne(e){return ue`
      <td class="total-row">${me(ge.total)}</td>
      <td class="total-row">${e.clusterCumulativeScore.toFixed(4)}</td>`}#oe(e,t,n){if(!t)return null;const o=e.args.data?.navigationId??i.Types.Events.NO_NAVIGATION,s=t.get(o)?.model.CLSCulprits;if(!s||s instanceof Error)return null;const a=s.shifts.get(e),l=e.args.data?.impacted_nodes??[],d=a&&(a.fontRequests.length||a.iframeIds.length||a.nonCompositedAnimations.length||a.unsizedImages.length),c=l?.length,h=s.clusters.find((t=>t.events.find((t=>t===e))));return ue`
      <table class="layout-shift-details-table">
        <thead class="table-title">
          <tr>
            <th>${me(ge.startTime)}</th>
            <th>${me(ge.shiftScore)}</th>
            ${c&&this.#W?ue`
              <th>${me(ge.elementsShifted)}</th>`:r.nothing}
            ${d&&this.#W?ue`
              <th>${me(ge.culprit)}</th> `:r.nothing}
          </tr>
        </thead>
        <tbody>
          ${this.#te(e,n,l,a)}
        </tbody>
      </table>
      ${this.#ie(h,n)}
    `}#re(e,t,n){if(!t)return null;const o=e.navigationId??i.Types.Events.NO_NAVIGATION,s=t.get(o)?.model.CLSCulprits;if(!s||s instanceof Error)return null;const a=Array.from(s.shifts.entries()).filter((([t])=>e.events.includes(t))).map((([,e])=>e)).flatMap((e=>Object.values(e))).flat(),l=Boolean(a.length);return ue`
          <table class="layout-shift-details-table">
            <thead class="table-title">
              <tr>
                <th>${me(ge.startTime)}</th>
                <th>${me(ge.shiftScore)}</th>
                ${this.#W?ue`
                  <th>${me(ge.elementsShifted)}</th>`:r.nothing}
                ${l&&this.#W?ue`
                  <th>${me(ge.culprit)}</th> `:r.nothing}
              </tr>
            </thead>
            <tbody>
              ${e.events.map((e=>{const t=s.shifts.get(e),i=e.args.data?.impacted_nodes??[];return this.#te(e,n,i,t)}))}
              ${this.#ne(e)}
            </tbody>
          </table>
        `}#i(){if(!this.#B||!this.#V)return;const e=ue`
      <div class="layout-shift-summary-details">
        <div
          class="event-details"
          @mouseover=${this.#se}
          @mouseleave=${this.#se}
        >
          ${this.#j(this.#B)}
          ${i.Types.Events.isSyntheticLayoutShift(this.#B)?this.#oe(this.#B,this.#q,this.#V):this.#re(this.#B,this.#q,this.#V)}
        </div>
      </div>
    `;r.render(e,this.#e,{host:this})}#se(e){const t="mouseover"===e.type;if("mouseleave"===e.type&&this.dispatchEvent(new CustomEvent("toggle-popover",{detail:{show:t},bubbles:!0,composed:!0})),!(e.target instanceof HTMLElement&&this.#B))return;const n=e.target.closest("tbody tr");if(!n||!n.parentElement)return;const o=i.Types.Events.isSyntheticLayoutShift(this.#B)?this.#B:this.#B.events.find((e=>e.ts===parseInt(n.getAttribute("data-ts")??"",10)));this.dispatchEvent(new CustomEvent("toggle-popover",{detail:{event:o,show:t},bubbles:!0,composed:!0}))}}customElements.define("devtools-performance-layout-shift-details",ve);var be=Object.freeze({__proto__:null,LayoutShiftDetails:ve});const fe=new CSSStyleSheet;fe.replaceSync(":host{display:inline-block;max-width:100%;min-width:50px}devtools-select-menu{max-width:100%}\n/*# sourceURL=networkThrottlingSelector.css */\n");const{html:ye,nothing:we}=r,Se={network:"Network: {PH1}",networkThrottling:"Network throttling: {PH1}",disabled:"Disabled",presets:"Presets",custom:"Custom",add:"Add…"},ke=t.i18n.registerUIStrings("panels/timeline/components/NetworkThrottlingSelector.ts",Se),xe=t.i18n.getLocalizedString.bind(void 0,ke);class Ce extends HTMLElement{#e=this.attachShadow({mode:"open"});#ae;#le=[];#de;constructor(){super(),this.#ae=v.Settings.Settings.instance().moduleSetting("custom-network-conditions"),this.#ce(),this.#de=a.NetworkManager.MultitargetNetworkManager.instance().networkConditions(),this.#i()}connectedCallback(){this.#e.adoptedStyleSheets=[fe],a.NetworkManager.MultitargetNetworkManager.instance().addEventListener("ConditionsChanged",this.#he,this),this.#he(),this.#ae.addChangeListener(this.#ue,this)}disconnectedCallback(){a.NetworkManager.MultitargetNetworkManager.instance().removeEventListener("ConditionsChanged",this.#he,this),this.#ae.removeChangeListener(this.#ue,this)}#ce(){this.#le=[{name:xe(Se.disabled),items:[a.NetworkManager.NoThrottlingConditions]},{name:xe(Se.presets),items:l.ThrottlingPresets.ThrottlingPresets.networkPresets},{name:xe(Se.custom),items:this.#ae.get(),showCustomAddOption:!0,jslogContext:"custom-network-throttling-item"}]}#he(){this.#de=a.NetworkManager.MultitargetNetworkManager.instance().networkConditions(),n.ScheduledRender.scheduleRender(this,this.#i)}#h(e){const t=this.#le.flatMap((e=>e.items)).find((t=>this.#ge(t)===e.itemValue));t&&a.NetworkManager.MultitargetNetworkManager.instance().setNetworkConditions(t)}#ue(){this.#ce(),n.ScheduledRender.scheduleRender(this,this.#i)}#pe(e){return e.title instanceof Function?e.title():e.title}#me(){v.Revealer.reveal(this.#ae)}#ge(e){return e.i18nTitleKey||this.#pe(e)}#i=()=>{const e=this.#pe(this.#de),t=this.#ge(this.#de),i=ye`
      <devtools-select-menu
        @selectmenuselected=${this.#h}
        .showDivider=${!0}
        .showArrow=${!0}
        .sideButton=${!1}
        .showSelectedItem=${!0}
        .showConnector=${!1}
        .jslogContext=${"network-conditions"}
        .buttonTitle=${xe(Se.network,{PH1:e})}
        title=${xe(Se.networkThrottling,{PH1:e})}
      >
        ${this.#le.map((e=>ye`
            <devtools-menu-group .name=${e.name}>
              ${e.items.map((i=>{const n=this.#ge(i),o=this.#pe(i),r=e.jslogContext||d.StringUtilities.toKebabCase(i.i18nTitleKey||o);return ye`
                  <devtools-menu-item
                    title=${o}
                    .value=${n}
                    .selected=${t===n}
                    jslog=${s.item(r).track({click:!0})}
                  >
                    ${o}
                  </devtools-menu-item>
                `}))}
              ${e.showCustomAddOption?ye`
                <devtools-menu-item
                  .value=${1}
                  jslog=${s.action("add").track({click:!0})}
                  @click=${this.#me}
                >
                  ${xe(Se.add)}
                </devtools-menu-item>
              `:we}
            </devtools-menu-group>
          `))}
      </devtools-select-menu>
    `;r.render(i,this.#e,{host:this})}}customElements.define("devtools-network-throttling-selector",Ce);var Pe=Object.freeze({__proto__:null,NetworkThrottlingSelector:Ce});const $e=new CSSStyleSheet;$e.replaceSync(".metric-card{border-radius:var(--sys-shape-corner-small);padding:14px 16px;background-color:var(--sys-color-surface3);height:100%;box-sizing:border-box;&:not(:hover) .title-help{visibility:hidden}}.title{display:flex;justify-content:space-between;font-size:var(--sys-typescale-headline5-size);line-height:var(--sys-typescale-headline5-line-height);font-weight:var(--ref-typeface-weight-medium);margin:0;margin-bottom:6px}.title-help{height:var(--sys-typescale-headline5-line-height);margin-left:4px}.metric-values-section{position:relative;display:flex;column-gap:8px;margin-bottom:8px}.metric-values-section:focus-visible{outline:2px solid -webkit-focus-ring-color}.metric-source-block{flex:1}.metric-source-value{font-size:32px;line-height:36px;font-weight:var(--ref-typeface-weight-regular)}.metric-source-label{font-weight:var(--ref-typeface-weight-medium)}.good-bg{background-color:var(--app-color-performance-good)}.needs-improvement-bg{background-color:var(--app-color-performance-ok)}.poor-bg{background-color:var(--app-color-performance-bad)}.divider{width:100%;border:0;border-bottom:1px solid var(--sys-color-divider);margin:8px 0;box-sizing:border-box}.compare-text{margin-top:8px}.environment-recs-intro{margin-top:8px}.environment-recs{margin:12px 0}.environment-recs > summary{font-weight:var(--ref-typeface-weight-medium);margin-bottom:4px}.environment-recs-list{margin:0;padding-left:20px}.detailed-compare-text{margin-bottom:8px}.bucket-summaries{margin-top:8px;overflow-x:auto}.bucket-summaries.histogram{display:grid;grid-template-columns:minmax(min-content,auto) minmax(20px,50px) max-content;grid-auto-rows:1fr;column-gap:8px;justify-items:flex-end;align-items:center}.bucket-label{justify-self:start;font-weight:var(--ref-typeface-weight-medium)}.bucket-range{color:var(--sys-color-token-subtle)}.histogram-bar{height:6px}.histogram-percent{color:var(--sys-color-token-subtle);font-weight:var(--ref-typeface-weight-medium)}.tooltip{display:none;visibility:hidden;transition-property:visibility;width:min(var(--tooltip-container-width,350px),350px);max-width:max-content;position:absolute;top:100%;left:50%;transform:translateX(-50%);z-index:1;box-sizing:border-box;padding:var(--sys-size-5) var(--sys-size-6);border-radius:var(--sys-shape-corner-small);background-color:var(--sys-color-cdt-base-container);box-shadow:var(--drop-shadow-depth-3)}.phase-table-row{display:flex;justify-content:space-between}.phase-table-header-row{font-weight:var(--ref-typeface-weight-medium)}\n/*# sourceURL=metricCard.css */\n");const Te={goodBetterCompare:"Your local {PH1} value of {PH2} is good, but is significantly better than your users’ experience.",goodWorseCompare:"Your local {PH1} value of {PH2} is good, but is significantly worse than your users’ experience.",goodSimilarCompare:"Your local {PH1} value of {PH2} is good, and is similar to your users’ experience.",goodSummarized:"Your local {PH1} value of {PH2} is good.",needsImprovementBetterCompare:"Your local {PH1} value of {PH2} needs improvement, but is significantly better than your users’ experience.",needsImprovementWorseCompare:"Your local {PH1} value of {PH2} needs improvement, but is significantly worse than your users’ experience.",needsImprovementSimilarCompare:"Your local {PH1} value of {PH2} needs improvement, and is similar to your users’ experience.",needsImprovementSummarized:"Your local {PH1} value of {PH2} needs improvement.",poorBetterCompare:"Your local {PH1} value of {PH2} is poor, but is significantly better than your users’ experience.",poorWorseCompare:"Your local {PH1} value of {PH2} is poor, but is significantly worse than your users’ experience.",poorSimilarCompare:"Your local {PH1} value of {PH2} is poor, and is similar to your users’ experience.",poorSummarized:"Your local {PH1} value of {PH2} is poor.",goodGoodDetailedCompare:"Your local {PH1} value of {PH2} is good and is rated the same as {PH4} of real-user {PH1} experiences. Additionally, the field data 75th percentile {PH1} value of {PH3} is good.",goodNeedsImprovementDetailedCompare:"Your local {PH1} value of {PH2} is good and is rated the same as {PH4} of real-user {PH1} experiences. However, the field data 75th percentile {PH1} value of {PH3} needs improvement.",goodPoorDetailedCompare:"Your local {PH1} value of {PH2} is good and is rated the same as {PH4} of real-user {PH1} experiences. However, the field data 75th percentile {PH1} value of {PH3} is poor.",needsImprovementGoodDetailedCompare:"Your local {PH1} value of {PH2} needs improvement and is rated the same as {PH4} of real-user {PH1} experiences. However, the field data 75th percentile {PH1} value of {PH3} is good.",needsImprovementNeedsImprovementDetailedCompare:"Your local {PH1} value of {PH2} needs improvement and is rated the same as {PH4} of real-user {PH1} experiences. Additionally, the field data 75th percentile {PH1} value of {PH3} needs improvement.",needsImprovementPoorDetailedCompare:"Your local {PH1} value of {PH2} needs improvement and is rated the same as {PH4} of real-user {PH1} experiences. However, the field data 75th percentile {PH1} value of {PH3} is poor.",poorGoodDetailedCompare:"Your local {PH1} value of {PH2} is poor and is rated the same as {PH4} of real-user {PH1} experiences. However, the field data 75th percentile {PH1} value of {PH3} is good.",poorNeedsImprovementDetailedCompare:"Your local {PH1} value of {PH2} is poor and is rated the same as {PH4} of real-user {PH1} experiences. However, the field data 75th percentile {PH1} value of {PH3} needs improvement.",poorPoorDetailedCompare:"Your local {PH1} value of {PH2} is poor and is rated the same as {PH4} of real-user {PH1} experiences. Additionally, the field data 75th percentile {PH1} value of {PH3} is poor."},Le=t.i18n.registerUIStrings("panels/timeline/components/MetricCompareStrings.ts",Te);const Re=new CSSStyleSheet;Re.replaceSync(".metric-value{text-wrap:nowrap}.metric-value.dim{font-weight:var(--ref-typeface-weight-medium)}.metric-value.waiting{color:var(--sys-color-token-subtle)}.metric-value.good{color:var(--app-color-performance-good)}.metric-value.needs-improvement{color:var(--app-color-performance-ok)}.metric-value.poor{color:var(--app-color-performance-bad)}.metric-value.good.dim{color:var(--app-color-performance-good-dim)}.metric-value.needs-improvement.dim{color:var(--app-color-performance-ok-dim)}.metric-value.poor.dim{color:var(--app-color-performance-bad-dim)}\n/*# sourceURL=metricValueStyles.css */\n");const Ie={fms:"{PH1}[ms]()",fs:"{PH1}[s]()"},Ee=t.i18n.registerUIStrings("panels/timeline/components/Utils.ts",Ie),Me=t.i18n.getLocalizedString.bind(void 0,Ee);var De;function He(e){const{mimeType:t}=e.args.data;switch(e.args.data.resourceType){case"Document":return De.DOC;case"Stylesheet":return De.CSS;case"Image":return De.IMG;case"Media":return De.MEDIA;case"Font":return De.FONT;case"Script":case"WebSocket":return De.JS;default:return t.endsWith("/css")?De.CSS:t.endsWith("javascript")?De.JS:t.startsWith("image/")?De.IMG:t.startsWith("audio/")||t.startsWith("video/")?De.MEDIA:t.startsWith("font/")||t.includes("font-")?De.FONT:"application/wasm"===t?De.WASM:t.startsWith("text/")?De.DOC:De.OTHER}}function Fe(e){let t="--app-color-system";switch(e){case De.DOC:t="--app-color-doc";break;case De.JS:t="--app-color-scripting";break;case De.CSS:t="--app-color-css";break;case De.IMG:t="--app-color-image";break;case De.MEDIA:t="--app-color-media";break;case De.FONT:t="--app-color-font";break;case De.WASM:t="--app-color-wasm";break;case De.OTHER:default:t="--app-color-system"}return f.ThemeSupport.instance().getComputedValue(t)}function Oe(e){return Fe(He(e))}!function(e){e.DOC="Doc",e.CSS="CSS",e.JS="JS",e.FONT="Font",e.IMG="Img",e.MEDIA="Media",e.WASM="Wasm",e.OTHER="Other"}(De||(De={}));const Ne=[2500,4e3],Ae=[.1,.25],ze=[200,500];function Ue(e,t){return e<=t[0]?"good":e<=t[1]?"needs-improvement":"poor"}function _e(e,t,i,n,o){const r=document.createElement("span");if(r.classList.add("metric-value"),void 0===t)return r.classList.add("waiting"),r.textContent="-",r;r.textContent=n(t);const a=Ue(t,i);return r.classList.add(a),r.setAttribute("jslog",`${s.section(e)}`),o?.dim&&r.classList.add("dim"),r}var Be;!function(e){function i(e){const t=e.indexOf("["),i=-1!==t&&e.indexOf("]",t),n=i&&e.indexOf("(",i),o=n&&e.indexOf(")",n);if(!o||-1===o)return null;return{firstPart:e.substring(0,t),unitPart:e.substring(t+1,i),lastPart:e.substring(o+1)}}e.parse=i,e.formatMicroSecondsAsSeconds=function(e){const n=document.createElement("span");n.classList.add("number-with-unit");const o=d.Timing.microSecondsToMilliSeconds(e),r=d.Timing.milliSecondsToSeconds(o),s=Me(Ie.fs,{PH1:r.toFixed(2)}),a=i(s);if(!a)return n.textContent=t.TimeUtilities.formatMicroSecondsAsSeconds(e),{text:s,element:n};const{firstPart:l,unitPart:c,lastPart:h}=a;return l&&n.append(l),n.createChild("span","unit").textContent=c,h&&n.append(h),{text:n.textContent??"",element:n}},e.formatMicroSecondsAsMillisFixed=function(e,n=0){const o=document.createElement("span");o.classList.add("number-with-unit");const r=d.Timing.microSecondsToMilliSeconds(e),s=Me(Ie.fms,{PH1:r.toFixed(n)}),a=i(s);if(!a)return o.textContent=t.TimeUtilities.formatMicroSecondsAsMillisFixed(e),{text:s,element:o};const{firstPart:l,unitPart:c,lastPart:h}=a;return l&&o.append(l),o.createChild("span","unit").textContent=c,h&&o.append(h),{text:o.textContent??"",element:o}}}(Be||(Be={}));var qe=Object.freeze({__proto__:null,CLS_THRESHOLDS:Ae,INP_THRESHOLDS:ze,LCP_THRESHOLDS:Ne,get NetworkCategory(){return De},get NumberWithUnit(){return Be},colorForNetworkCategory:Fe,colorForNetworkRequest:Oe,networkResourceCategory:He,rateMetric:Ue,renderMetricValue:_e});const{html:Ve,nothing:We}=r,je={localValue:"Local",field75thPercentile:"Field 75th percentile",good:"Good",needsImprovement:"Needs improvement",poor:"Poor",leqRange:"(≤{PH1})",betweenRange:"({PH1}-{PH2})",gtRange:"(>{PH1})",percentage:"{PH1}%",interactToMeasure:"Interact with the page to measure INP.",viewCardDetails:"View card details",considerTesting:"Consider your local test conditions",recThrottlingLCP:"Real users may experience longer page loads due to slower network conditions. Increasing network throttling will simulate slower network conditions.",recThrottlingINP:"Real users may experience longer interactions due to slower CPU speeds. Increasing CPU throttling will simulate a slower device.",recViewportLCP:"Screen size can influence what the LCP element is. Ensure you are testing common viewport sizes.",recViewportCLS:"Screen size can influence what layout shifts happen. Ensure you are testing common viewport sizes.",recJourneyCLS:"How a user interacts with the page can influence layout shifts. Ensure you are testing common interactions like scrolling the page.",recJourneyINP:"How a user interacts with the page influences interaction delays. Ensure you are testing common interactions.",recDynamicContentLCP:"The LCP element can vary between page loads if content is dynamic.",recDynamicContentCLS:"Dynamic content can influence what layout shifts happen.",phase:"Phase",duration:"Local duration (ms)",lcpHelpTooltip:"LCP reports the render time of the largest image, text block, or video visible in the viewport. Click here to learn more about LCP.",clsHelpTooltip:"CLS measures the amount of unexpected shifted content. Click here to learn more about CLS.",inpHelpTooltip:"INP measures the overall responsiveness to all click, tap, and keyboard interactions. Click here to learn more about INP."},Ge=t.i18n.registerUIStrings("panels/timeline/components/MetricCard.ts",je),Ke=t.i18n.getLocalizedString.bind(void 0,Ge);class Ye extends HTMLElement{#e=this.attachShadow({mode:"open"});constructor(){super(),this.#i()}#ve;#be={metric:"LCP"};set data(e){this.#be=e,n.ScheduledRender.scheduleRender(this,this.#i)}connectedCallback(){this.#e.adoptedStyleSheets=[$e,Re],n.ScheduledRender.scheduleRender(this,this.#i)}#fe=e=>{d.KeyboardUtilities.isEscKey(e)&&(e.stopPropagation(),this.#ye())};#we(e){const t=e.target;t?.hasFocus()||this.#ye()}#Se(e){const t=e.target;if(t?.hasFocus())return;const i=e.relatedTarget;i instanceof Node&&t.contains(i)||this.#ye()}#ye(){const e=this.#ve;e&&(document.body.removeEventListener("keydown",this.#fe),e.style.removeProperty("left"),e.style.removeProperty("visibility"),e.style.removeProperty("display"),e.style.removeProperty("transition-delay"))}#ke(e=0){const t=this.#ve;if(!t||t.style.visibility||t.style.display)return;document.body.addEventListener("keydown",this.#fe),t.style.display="block",t.style.transitionDelay=`${Math.round(e)}ms`;const i=this.#be.tooltipContainer;if(!i)return;const n=i.getBoundingClientRect();t.style.setProperty("--tooltip-container-width",`${Math.round(n.width)}px`),requestAnimationFrame((()=>{let e=0;const i=t.getBoundingClientRect(),o=i.right-n.right,r=i.left-n.left;r<0?e=Math.round(r):o>0&&(e=Math.round(o)),t.style.left=`calc(50% - ${e}px)`,t.style.visibility="visible"}))}#xe(){switch(this.#be.metric){case"LCP":return 1e3;case"CLS":return.1;case"INP":return 200}}#Ce(){switch(this.#be.metric){case"LCP":return t.i18n.lockedString("Largest Contentful Paint (LCP)");case"CLS":return t.i18n.lockedString("Cumulative Layout Shift (CLS)");case"INP":return t.i18n.lockedString("Interaction to Next Paint (INP)")}}#Pe(){switch(this.#be.metric){case"LCP":return Ne;case"CLS":return Ae;case"INP":return ze}}#$e(){switch(this.#be.metric){case"LCP":return e=>{const i=1e3*e;return t.TimeUtilities.formatMicroSecondsAsSeconds(i)};case"CLS":return e=>0===e?"0":e.toFixed(2);case"INP":return e=>t.TimeUtilities.preciseMillisToString(e)}}#Te(){switch(this.#be.metric){case"LCP":return"https://web.dev/articles/lcp";case"CLS":return"https://web.dev/articles/cls";case"INP":return"https://web.dev/articles/inp"}}#Le(){switch(this.#be.metric){case"LCP":return Ke(je.lcpHelpTooltip);case"CLS":return Ke(je.clsHelpTooltip);case"INP":return Ke(je.inpHelpTooltip)}}#Re(){const{localValue:e}=this.#be;if(void 0!==e)return e}#Ie(){let{fieldValue:e}=this.#be;if(void 0!==e&&("string"==typeof e&&(e=Number(e)),Number.isFinite(e)))return e}#Ee(){const e=this.#Re(),t=this.#Ie();if(void 0===e||void 0===t)return;const i=this.#Pe(),n=Ue(e,i),o=Ue(t,i);if("good"===n&&"good"===o)return"similar";const r=this.#xe();return e-t>r?"worse":t-e>r?"better":"similar"}#Me(){const e=this.#Re();if(void 0===e)return"INP"===this.#be.metric?Ve`
          <div class="compare-text">${Ke(je.interactToMeasure)}</div>
        `:r.nothing;const i=this.#Ee(),n=Ue(e,this.#Pe()),o=_e(this.#De(!0),e,this.#Pe(),this.#$e(),{dim:!0});return Ve`
      <div class="compare-text">
        ${function(e){const{rating:i,compare:n}=e,o={PH1:e.metric,PH2:e.localValue};if("good"===i&&"better"===n)return t.i18n.getFormatLocalizedString(Le,Te.goodBetterCompare,o);if("good"===i&&"worse"===n)return t.i18n.getFormatLocalizedString(Le,Te.goodWorseCompare,o);if("good"===i&&"similar"===n)return t.i18n.getFormatLocalizedString(Le,Te.goodSimilarCompare,o);if("good"===i&&!n)return t.i18n.getFormatLocalizedString(Le,Te.goodSummarized,o);if("needs-improvement"===i&&"better"===n)return t.i18n.getFormatLocalizedString(Le,Te.needsImprovementBetterCompare,o);if("needs-improvement"===i&&"worse"===n)return t.i18n.getFormatLocalizedString(Le,Te.needsImprovementWorseCompare,o);if("needs-improvement"===i&&"similar"===n)return t.i18n.getFormatLocalizedString(Le,Te.needsImprovementSimilarCompare,o);if("needs-improvement"===i&&!n)return t.i18n.getFormatLocalizedString(Le,Te.needsImprovementSummarized,o);if("poor"===i&&"better"===n)return t.i18n.getFormatLocalizedString(Le,Te.poorBetterCompare,o);if("poor"===i&&"worse"===n)return t.i18n.getFormatLocalizedString(Le,Te.poorWorseCompare,o);if("poor"===i&&"similar"===n)return t.i18n.getFormatLocalizedString(Le,Te.poorSimilarCompare,o);if("poor"===i&&!n)return t.i18n.getFormatLocalizedString(Le,Te.poorSummarized,o);throw new Error("Compare string not found")}({metric:t.i18n.lockedString(this.#be.metric),rating:n,compare:i,localValue:o})}
      </div>
    `}#He(){const e=this.#Ee();if(!e||"similar"===e)return r.nothing;const t=[],i=this.#be.metric;return"LCP"===i&&"better"===e?t.push(Ke(je.recThrottlingLCP)):"INP"===i&&"better"===e&&t.push(Ke(je.recThrottlingINP)),"LCP"===i?t.push(Ke(je.recViewportLCP)):"CLS"===i&&t.push(Ke(je.recViewportCLS)),"CLS"===i?t.push(Ke(je.recJourneyCLS)):"INP"===i&&t.push(Ke(je.recJourneyINP)),"LCP"===i?t.push(Ke(je.recDynamicContentLCP)):"CLS"===i&&t.push(Ke(je.recDynamicContentCLS)),t.length?Ve`
      <details class="environment-recs">
        <summary>${Ke(je.considerTesting)}</summary>
        <ul class="environment-recs-list">${t.map((e=>Ve`<li>${e}</li>`))}</ul>
      </details>
    `:r.nothing}#De(e){return`timeline.landing.${e?"local":"field"}-${this.#be.metric.toLowerCase()}`}#Fe(){const e=this.#Re();if(void 0===e)return"INP"===this.#be.metric?Ve`
          <div class="detailed-compare-text">${Ke(je.interactToMeasure)}</div>
        `:r.nothing;const i=Ue(e,this.#Pe()),n=this.#Ie(),o=void 0!==n?Ue(n,this.#Pe()):void 0,s=_e(this.#De(!0),e,this.#Pe(),this.#$e(),{dim:!0}),a=_e(this.#De(!1),n,this.#Pe(),this.#$e(),{dim:!0});return Ve`
      <div class="detailed-compare-text">${function(e){const{localRating:i,fieldRating:n}=e,o={PH1:e.metric,PH2:e.localValue,PH3:e.fieldValue,PH4:e.percent};if("good"===i&&"good"===n)return t.i18n.getFormatLocalizedString(Le,Te.goodGoodDetailedCompare,o);if("good"===i&&"needs-improvement"===n)return t.i18n.getFormatLocalizedString(Le,Te.goodNeedsImprovementDetailedCompare,o);if("good"===i&&"poor"===n)return t.i18n.getFormatLocalizedString(Le,Te.goodPoorDetailedCompare,o);if("good"===i&&!n)return t.i18n.getFormatLocalizedString(Le,Te.goodSummarized,o);if("needs-improvement"===i&&"good"===n)return t.i18n.getFormatLocalizedString(Le,Te.needsImprovementGoodDetailedCompare,o);if("needs-improvement"===i&&"needs-improvement"===n)return t.i18n.getFormatLocalizedString(Le,Te.needsImprovementNeedsImprovementDetailedCompare,o);if("needs-improvement"===i&&"poor"===n)return t.i18n.getFormatLocalizedString(Le,Te.needsImprovementPoorDetailedCompare,o);if("needs-improvement"===i&&!n)return t.i18n.getFormatLocalizedString(Le,Te.needsImprovementSummarized,o);if("poor"===i&&"good"===n)return t.i18n.getFormatLocalizedString(Le,Te.poorGoodDetailedCompare,o);if("poor"===i&&"needs-improvement"===n)return t.i18n.getFormatLocalizedString(Le,Te.poorNeedsImprovementDetailedCompare,o);if("poor"===i&&"poor"===n)return t.i18n.getFormatLocalizedString(Le,Te.poorPoorDetailedCompare,o);if("poor"===i&&!n)return t.i18n.getFormatLocalizedString(Le,Te.poorSummarized,o);throw new Error("Detailed compare string not found")}({metric:t.i18n.lockedString(this.#be.metric),localRating:i,fieldRating:o,localValue:s,fieldValue:a,percent:this.#Oe(i)})}</div>
    `}#Ne(e){switch(e){case"good":return 0;case"needs-improvement":return 1;case"poor":return 2}}#Ae(e){const t=this.#be.histogram,i=t?.[this.#Ne(e)].density||0;return`${Math.round(100*i)}%`}#Oe(e){const t=this.#be.histogram;if(void 0===t)return"-";const i=t[this.#Ne(e)].density||0,n=Math.round(100*i);return Ke(je.percentage,{PH1:n})}#ze(){const e=c.CrUXManager.instance().getConfigSetting().get().enabled,t=this.#$e(),i=this.#Pe(),n=Ve`
      <div class="bucket-label">
        <span>${Ke(je.good)}</span>
        <span class="bucket-range">${Ke(je.leqRange,{PH1:t(i[0])})}</span>
      </div>
    `,o=Ve`
      <div class="bucket-label">
        <span>${Ke(je.needsImprovement)}</span>
        <span class="bucket-range">${Ke(je.betweenRange,{PH1:t(i[0]),PH2:t(i[1])})}</span>
      </div>
    `,r=Ve`
      <div class="bucket-label">
        <span>${Ke(je.poor)}</span>
        <span class="bucket-range">${Ke(je.gtRange,{PH1:t(i[1])})}</span>
      </div>
    `;return e?Ve`
      <div class="bucket-summaries histogram">
        ${n}
        <div class="histogram-bar good-bg" style="width: ${this.#Ae("good")}"></div>
        <div class="histogram-percent">${this.#Oe("good")}</div>
        ${o}
        <div class="histogram-bar needs-improvement-bg" style="width: ${this.#Ae("needs-improvement")}"></div>
        <div class="histogram-percent">${this.#Oe("needs-improvement")}</div>
        ${r}
        <div class="histogram-bar poor-bg" style="width: ${this.#Ae("poor")}"></div>
        <div class="histogram-percent">${this.#Oe("poor")}</div>
      </div>
    `:Ve`
        <div class="bucket-summaries">
          ${n}
          ${o}
          ${r}
        </div>
      `}#Ue(){const e=this.#Re(),t=this.#be.phases;return t&&e?Ve`
      <hr class="divider">
      <div class="phase-table" role="table">
        <div class="phase-table-row phase-table-header-row" role="row">
          <div role="columnheader">${Ke(je.phase)}</div>
          <div role="columnheader">${Ke(je.duration)}</div>
        </div>
        ${t.map((e=>Ve`
          <div class="phase-table-row" role="row">
            <div role="cell">${e[0]}</div>
            <div role="cell">${Math.round(e[1])}</div>
          </div>
        `))}
      </div>
    `:r.nothing}#i=()=>{const e=c.CrUXManager.instance().getConfigSetting().get().enabled,t=this.#Te(),i=Ve`
      <div class="metric-card">
        <h3 class="title">
          ${this.#Ce()}
          <devtools-button
            class="title-help"
            title=${this.#Le()}
            .iconName=${"help"}
            .variant=${"icon"}
            @click=${()=>b.InspectorFrontendHost.InspectorFrontendHostInstance.openInNewTab(t)}
          ></devtools-button>
        </h3>
        <div tabindex="0" class="metric-values-section"
          @mouseenter=${()=>this.#ke(500)}
          @mouseleave=${this.#we}
          @focusin=${this.#ke}
          @focusout=${this.#Se}
          aria-describedby="tooltip"
        >
          <div class="metric-source-block">
            <div class="metric-source-value" id="local-value">${_e(this.#De(!0),this.#Re(),this.#Pe(),this.#$e())}</div>
            ${e?Ve`<div class="metric-source-label">${Ke(je.localValue)}</div>`:We}
          </div>
          ${e?Ve`
            <div class="metric-source-block">
              <div class="metric-source-value" id="field-value">${_e(this.#De(!1),this.#Ie(),this.#Pe(),this.#$e())}</div>
              <div class="metric-source-label">${Ke(je.field75thPercentile)}</div>
            </div>
          `:We}
          <div
            id="tooltip"
            class="tooltip"
            role="tooltip"
            aria-label=${Ke(je.viewCardDetails)}
            on-render=${n.Directives.nodeRenderedCallback((e=>{this.#ve=e}))}
          >
            ${this.#Fe()}
            <hr class="divider">
            ${this.#ze()}
            ${this.#Ue()}
          </div>
        </div>
        ${e?Ve`<hr class="divider">`:We}
        ${this.#Me()}
        ${this.#He()}
        <slot name="extra-info"></slot>
      </div>
    `;r.render(i,this.#e,{host:this})}}customElements.define("devtools-metric-card",Ye);var Xe=Object.freeze({__proto__:null,MetricCard:Ye});const Je=new CSSStyleSheet;Je.replaceSync('.container{container-type:inline-size;height:100%;font-size:var(--sys-typescale-body4-size);line-height:var(--sys-typescale-body4-line-height);font-weight:var(--ref-typeface-weight-regular);user-select:text}.live-metrics-view{--min-main-area-size:60%;background-color:var(--sys-color-cdt-base-container);display:flex;flex-direction:row;width:100%;height:100%}.live-metrics,\n.next-steps{padding:16px;height:100%;overflow-y:auto;box-sizing:border-box}.live-metrics{flex:1;display:flex;flex-direction:column}.next-steps{flex:0 0 336px;box-sizing:border-box;border:none;border-left:1px solid var(--sys-color-divider)}@container (max-width: 650px){.live-metrics-view{flex-direction:column}.next-steps{flex-basis:40%;border:none;border-top:1px solid var(--sys-color-divider)}}.metric-cards{display:grid;gap:16px;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));width:100%}.section-title{font-size:var(--sys-typescale-headline4-size);line-height:var(--sys-typescale-headline4-line-height);font-weight:var(--ref-typeface-weight-medium);margin:0;margin-bottom:10px}.settings-card{border-radius:var(--sys-shape-corner-small);padding:14px 16px 16px;background-color:var(--sys-color-surface3);margin-bottom:16px}.record-action-card{border-radius:var(--sys-shape-corner-small);padding:12px 16px 12px 12px;background-color:var(--sys-color-surface3);margin-bottom:16px}.card-title{font-size:var(--sys-typescale-headline5-size);line-height:var(--sys-typescale-headline5-line-height);font-weight:var(--ref-typeface-weight-medium);margin:0}.settings-card .card-title{margin-bottom:4px}.device-toolbar-description{margin-bottom:12px;display:flex}.network-cache-setting{display:inline-block;max-width:max-content}.throttling-recommendation-value{font-weight:var(--ref-typeface-weight-medium)}.related-info{text-wrap:nowrap;margin-top:8px;display:flex}.related-info-label{font-weight:var(--ref-typeface-weight-medium);margin-right:4px}.related-info-link{background-color:var(--sys-color-cdt-base-container);border-radius:2px;padding:0 2px;min-width:0}.local-field-link{margin-top:8px}.logs-section{margin-top:24px;display:flex;flex-direction:column;flex:1 0 300px;overflow:auto;max-height:max-content;--app-color-toolbar-background:transparent}.logs-section-header{display:flex;align-items:center}.interactions-clear{margin-left:4px;vertical-align:sub}.log{padding:0;margin:0;overflow:auto}.log-item{border:none;border-bottom:1px solid var(--sys-color-divider);&.highlight{animation:highlight-fadeout 2s}}.interaction{--phase-table-margin:120px;--details-indicator-width:18px;summary{display:flex;align-items:center;padding:7px 4px;&::before{content:" ";height:14px;width:var(--details-indicator-width);mask-image:var(--image-file-triangle-right);background-color:var(--icon-default);flex-shrink:0}}details[open] summary::before{mask-image:var(--image-file-triangle-down)}}.interaction-type{font-weight:var(--ref-typeface-weight-medium);width:calc(var(--phase-table-margin) - var(--details-indicator-width));flex-shrink:0}.interaction-inp-chip{background-color:var(--sys-color-yellow-bright);color:var(--sys-color-on-yellow);padding:0 2px}.interaction-node{flex-grow:1;margin-right:32px;min-width:0}.interaction-info{width:var(--sys-typescale-body4-line-height);height:var(--sys-typescale-body4-line-height);margin-right:6px}.interaction-duration{text-align:end;width:max-content;flex-shrink:0;font-weight:var(--ref-typeface-weight-medium)}.layout-shift{display:flex;align-items:flex-start}.layout-shift-score{margin-right:16px;padding:7px 0;width:150px;box-sizing:border-box}.layout-shift-nodes{flex:1;min-width:0}.layout-shift-node{border-bottom:1px solid var(--sys-color-divider);padding:7px 0;&:last-child{border:none}}.record-action{display:flex;flex-direction:row;align-items:center;justify-content:space-between;gap:8px}.shortcut-label{width:max-content;flex-shrink:0}.field-data-option{margin:8px 0;max-width:100%}.field-setup-buttons{margin-top:14px}.field-data-message{margin-bottom:12px}.collection-period-range{font-weight:var(--ref-typeface-weight-medium)}x-link{color:var(--sys-color-primary);text-decoration-line:underline}.environment-option{display:flex;align-items:center;margin-top:8px}.environment-recs-title{margin-top:12px;font-weight:var(--ref-typeface-weight-medium)}.environment-recs-list{margin:0;padding-left:20px}.environment-rec{font-weight:var(--ref-typeface-weight-medium)}.link-to-log{padding:unset;background:unset;border:unset;font:inherit;color:var(--sys-color-primary);text-decoration:underline;cursor:pointer}@keyframes highlight-fadeout{from{background-color:var(--sys-color-yellow-container)}to{background-color:transparent}}.phase-table{border-top:1px solid var(--sys-color-divider);padding:7px 4px;margin-left:var(--phase-table-margin)}.phase-table-row{display:flex;justify-content:space-between}.phase-table-header-row{font-weight:var(--ref-typeface-weight-medium);margin-bottom:4px}.log-extra-details-button{padding:unset;background:unset;border:unset;font:inherit;color:var(--sys-color-primary);text-decoration:underline;cursor:pointer}\n/*# sourceURL=liveMetricsView.css */\n');const{html:Ze,nothing:Qe,Directives:et}=r,{until:tt}=et,it=k.RenderCoordinator.RenderCoordinator.instance(),nt=["AUTO",...c.DEVICE_SCOPE_LIST],ot={localAndFieldMetrics:"Local and field metrics",localMetrics:"Local metrics",eventLogs:"Interaction and layout shift logs section",interactions:"Interactions",layoutShifts:"Layout shifts",nextSteps:"Next steps",fieldData:"Field data",environmentSettings:"Environment settings",showFieldDataForDevice:"Show field data for device type: {PH1}",notEnoughData:"Not enough data",network:"Network: {PH1}",device:"Device: {PH1}",allDevices:"All devices",desktop:"Desktop",mobile:"Mobile",tablet:"Tablet",auto:"Auto ({PH1})",loadingOption:"{PH1} - Loading…",needsDataOption:"{PH1} - No data",urlOption:"URL",originOption:"Origin",urlOptionWithKey:"URL: {PH1}",originOptionWithKey:"Origin: {PH1}",showFieldDataForPage:"Show field data for {PH1}",tryDisablingThrottling:"75th percentile is too fast to simulate with throttling",tryUsingThrottling:"75th percentile is similar to {PH1} throttling",percentDevices:"{PH1}% mobile, {PH2}% desktop",useDeviceToolbar:"Use the device toolbar to {PH1}.",simulateDifferentDevices:"simulate different devices",disableNetworkCache:"Disable network cache",lcpElement:"LCP element",inpInteractionLink:"INP interaction",worstCluster:"Worst cluster",numShifts:"{shiftCount, plural,\n    =1 {{shiftCount} shift}\n    other {{shiftCount} shifts}\n  }",collectionPeriod:"Collection period: {PH1}",dateRange:"{PH1} - {PH2}",seeHowYourLocalMetricsCompare:"See how your local metrics compare to real user data in the {PH1}.",localFieldLearnMoreLink:"Learn more about local and field data",localFieldLearnMoreTooltip:"Local metrics are captured from the current page using your network connection and device. Field data is measured by real users using many different network connections and devices.",interactionExcluded:"INP is calculated using the 98th percentile of interaction delays, so some interaction delays may be larger than the INP value.",clearCurrentLog:"Clear the current log",realUserEnvironments:"Real user environments",timeToFirstByte:"Time to first byte",resourceLoadDelay:"Resource load delay",resourceLoadDuration:"Resource load duration",elementRenderDelay:"Element render delay",inputDelay:"Input delay",processingDuration:"Processing duration",presentationDelay:"Presentation delay",inpInteraction:"The INP interaction is at the 98th percentile of interaction delays.",showInpInteraction:"Go to the INP interaction.",showClsCluster:"Go to worst layout shift cluster.",phase:"Phase",duration:"Local duration (ms)",logToConsole:"Log additional interaction data to the console"},rt=t.i18n.registerUIStrings("panels/timeline/components/LiveMetricsView.ts",ot),st=t.i18n.getLocalizedString.bind(void 0,rt);class at extends S.LegacyWrapper.WrappableComponent{#e=this.attachShadow({mode:"open"});#_e;#Be;#qe;#Ve=new Map;#We=[];#je;#Ge="AUTO";#Ke="url";#Ye;#Xe;#Je;#Ze;#Qe;#et;#tt=!1;constructor(){super(),this.#Ye=o.ActionRegistry.ActionRegistry.instance().getAction("timeline.toggle-recording"),this.#Xe=o.ActionRegistry.ActionRegistry.instance().getAction("timeline.record-reload")}#it(e){this.#_e=e.data.lcp,this.#Be=e.data.cls,this.#qe=e.data.inp;const t=this.#We.length<e.data.layoutShifts.length;this.#We=[...e.data.layoutShifts];const i=this.#Ve.size<e.data.interactions.size;this.#Ve=new Map(e.data.interactions);const o=n.ScheduledRender.scheduleRender(this,this.#i);i&&this.#Qe&&this.#nt(o,this.#Qe),t&&this.#et&&this.#nt(o,this.#et)}#nt(e,t){if(!t.checkVisibility())return;(Math.abs(t.scrollHeight-t.clientHeight-t.scrollTop)<=1||this.#tt)&&e.then((()=>{requestAnimationFrame((()=>{this.#tt=!0,t.addEventListener("scrollend",(()=>{this.#tt=!1}),{once:!0}),t.scrollTo({top:t.scrollHeight,behavior:"smooth"})}))}))}#ot(e){this.#je=e.data,n.ScheduledRender.scheduleRender(this,this.#i)}#rt(){n.ScheduledRender.scheduleRender(this,this.#i)}async#st(){this.#je=await c.CrUXManager.instance().getFieldDataForCurrentPage(),n.ScheduledRender.scheduleRender(this,this.#i)}#at(){const e="AUTO"===this.#Ge?this.#lt():this.#Ge;return this.#je?.[`${this.#Ke}-${e}`]}#dt(e){return this.#at()?.record.metrics[e]}connectedCallback(){this.#e.adoptedStyleSheets=[Je,Re];const e=w.LiveMetrics.instance();e.addEventListener("status",this.#it,this);const t=c.CrUXManager.instance();t.addEventListener("field-data-changed",this.#ot,this);const i=this.#ct();i?.addEventListener("Updated",this.#rt,this),t.getConfigSetting().get().enabled&&this.#st(),this.#_e=e.lcpValue,this.#Be=e.clsValue,this.#qe=e.inpValue,this.#Ve=e.interactions,this.#We=e.layoutShifts,n.ScheduledRender.scheduleRender(this,this.#i)}#ct(){try{return y.DeviceModeModel.DeviceModeModel.instance()}catch{return null}}disconnectedCallback(){w.LiveMetrics.instance().removeEventListener("status",this.#it,this);c.CrUXManager.instance().removeEventListener("field-data-changed",this.#ot,this),this.#ct()?.removeEventListener("Updated",this.#rt,this)}#ht(){const e=this.#dt("largest_contentful_paint"),t=this.#_e?.node,i=this.#_e?.phases;return Ze`
      <devtools-metric-card .data=${{metric:"LCP",localValue:this.#_e?.value,fieldValue:e?.percentiles?.p75,histogram:e?.histogram,tooltipContainer:this.#Ze,phases:i&&[[st(ot.timeToFirstByte),i.timeToFirstByte],[st(ot.resourceLoadDelay),i.resourceLoadDelay],[st(ot.resourceLoadDuration),i.resourceLoadTime],[st(ot.elementRenderDelay),i.elementRenderDelay]]}}>
        ${t?Ze`
            <div class="related-info" slot="extra-info">
              <span class="related-info-label">${st(ot.lcpElement)}</span>
              <span class="related-info-link">${tt(v.Linkifier.Linkifier.linkify(t))}</span>
            </div>
          `:Qe}
      </devtools-metric-card>
    `}#ut(){const e=this.#dt("cumulative_layout_shift"),t=new Set(this.#Be?.clusterShiftIds||[]),i=t.size>0&&this.#We.some((e=>t.has(e.uniqueLayoutShiftId)));return Ze`
      <devtools-metric-card .data=${{metric:"CLS",localValue:this.#Be?.value,fieldValue:e?.percentiles?.p75,histogram:e?.histogram,tooltipContainer:this.#Ze}}>
        ${i?Ze`
          <div class="related-info" slot="extra-info">
            <span class="related-info-label">${st(ot.worstCluster)}</span>
            <button
              class="link-to-log"
              title=${st(ot.showClsCluster)}
              @click=${()=>this.#gt(t)}
              jslog=${s.action("timeline.landing.show-cls-cluster").track({click:!0})}
            >${st(ot.numShifts,{shiftCount:t.size})}</button>
          </div>
        `:Qe}
      </devtools-metric-card>
    `}#pt(){const e=this.#dt("interaction_to_next_paint"),t=this.#qe?.phases,i=this.#qe&&this.#Ve.get(this.#qe.interactionId);return Ze`
      <devtools-metric-card .data=${{metric:"INP",localValue:this.#qe?.value,fieldValue:e?.percentiles?.p75,histogram:e?.histogram,tooltipContainer:this.#Ze,phases:t&&[[st(ot.inputDelay),t.inputDelay],[st(ot.processingDuration),t.processingDuration],[st(ot.presentationDelay),t.presentationDelay]]}}>
        ${i?Ze`
          <div class="related-info" slot="extra-info">
            <span class="related-info-label">${st(ot.inpInteractionLink)}</span>
            <button
              class="link-to-log"
              title=${st(ot.showInpInteraction)}
              @click=${()=>this.#mt(i)}
              jslog=${s.action("timeline.landing.show-inp-interaction").track({click:!0})}
            >${i.interactionType}</button>
          </div>
        `:Qe}
      </devtools-metric-card>
    `}#vt(e){return Ze`
      <div class="record-action">
        <devtools-button @click=${function(){e.execute()}} .data=${{variant:"text",size:"REGULAR",iconName:e.icon(),title:e.title(),jslogContext:e.id()}}>
          ${e.title()}
        </devtools-button>
        <span class="shortcut-label">${o.ShortcutRegistry.ShortcutRegistry.instance().shortcutTitleForAction(e.id())}</span>
      </div>
    `}#bt(){const e=this.#dt("round_trip_time");if(!e?.percentiles)return null;const t=Number(e.percentiles.p75);if(!Number.isFinite(t))return null;if(t<60)return st(ot.tryDisablingThrottling);let i=null,n=1/0;for(const e of l.ThrottlingPresets.ThrottlingPresets.networkPresets){const{targetLatency:o}=e;if(!o)continue;const r=Math.abs(o-t);r>200||(n<r||(i=e,n=r))}if(!i)return null;const o="function"==typeof i.title?i.title():i.title;return st(ot.tryUsingThrottling,{PH1:o})}#ft(){const e=this.#je?.[`${this.#Ke}-ALL`]?.record.metrics.form_factors?.fractions;return e?st(ot.percentDevices,{PH1:Math.round(100*e.phone),PH2:Math.round(100*e.desktop)}):null}#yt(){const e=c.CrUXManager.instance().getConfigSetting().get().enabled,i=o.XLink.XLink.create("https://developer.chrome.com/docs/devtools/device-mode",st(ot.simulateDifferentDevices)),n=t.i18n.getFormatLocalizedString(rt,ot.useDeviceToolbar,{PH1:i}),r=document.createElement("span");r.classList.add("environment-rec"),r.textContent=this.#ft()||st(ot.notEnoughData);const s=document.createElement("span");return s.classList.add("environment-rec"),s.textContent=this.#bt()||st(ot.notEnoughData),Ze`
      <h3 class="card-title">${st(ot.environmentSettings)}</h3>
      <div class="device-toolbar-description">${n}</div>
      ${e?Ze`
        <div class="environment-recs-title">${st(ot.realUserEnvironments)}</div>
        <ul class="environment-recs-list">
          <li>${t.i18n.getFormatLocalizedString(rt,ot.device,{PH1:r})}</li>
          <li>${t.i18n.getFormatLocalizedString(rt,ot.network,{PH1:s})}</li>
        </ul>
      `:Qe}
      <div class="environment-option">
        <devtools-cpu-throttling-selector></devtools-cpu-throttling-selector>
      </div>
      <div class="environment-option">
        <devtools-network-throttling-selector></devtools-network-throttling-selector>
      </div>
      <div class="environment-option">
        <setting-checkbox
          class="network-cache-setting"
          .data=${{setting:v.Settings.Settings.instance().moduleSetting("cache-disabled"),textOverride:st(ot.disableNetworkCache)}}
        ></setting-checkbox>
      </div>
    `}#wt(e){const t=this.#je?.[`${e}-ALL`]?.record.key[e];if(t)return st("url"===e?ot.urlOptionWithKey:ot.originOptionWithKey,{PH1:t});const i=st("url"===e?ot.urlOption:ot.originOption);return st(ot.needsDataOption,{PH1:i})}#St(e){"url"===e.itemValue?this.#Ke="url":this.#Ke="origin",n.ScheduledRender.scheduleRender(this,this.#i)}#kt(){if(!c.CrUXManager.instance().getConfigSetting().get().enabled)return r.nothing;const e=this.#wt("url"),t=this.#wt("origin"),i="url"===this.#Ke?e:t,n=st(ot.showFieldDataForPage,{PH1:i}),o=!this.#je?.["url-ALL"]&&!this.#je?.["origin-ALL"];return Ze`
      <devtools-select-menu
        id="page-scope-select"
        class="field-data-option"
        @selectmenuselected=${this.#St}
        .showDivider=${!0}
        .showArrow=${!0}
        .sideButton=${!1}
        .showSelectedItem=${!0}
        .showConnector=${!1}
        .buttonTitle=${i}
        .disabled=${o}
        title=${n}
      >
        <devtools-menu-item
          .value=${"url"}
          .selected=${"url"===this.#Ke}
        >
          ${e}
        </devtools-menu-item>
        <devtools-menu-item
          .value=${"origin"}
          .selected=${"origin"===this.#Ke}
        >
          ${t}
        </devtools-menu-item>
      </devtools-select-menu>
    `}#xt(e){switch(e){case"ALL":return st(ot.allDevices);case"DESKTOP":return st(ot.desktop);case"PHONE":return st(ot.mobile);case"TABLET":return st(ot.tablet)}}#lt(){const e=this.#ct();return null===e?"ALL":e.isMobile()?this.#je?.[`${this.#Ke}-PHONE`]?"PHONE":"ALL":this.#je?.[`${this.#Ke}-DESKTOP`]?"DESKTOP":"ALL"}#Ct(e){const t="AUTO"===e?this.#lt():e,i=this.#xt(t),n="AUTO"===e?st(ot.auto,{PH1:i}):i;if(!this.#je)return st(ot.loadingOption,{PH1:n});return this.#je[`${this.#Ke}-${t}`]?n:st(ot.needsDataOption,{PH1:n})}#Pt(e){this.#Ge=e.itemValue,n.ScheduledRender.scheduleRender(this,this.#i)}#$t(){if(!c.CrUXManager.instance().getConfigSetting().get().enabled)return r.nothing;const e=!this.#je?.[`${this.#Ke}-ALL`],t=this.#Ct(this.#Ge);return Ze`
      <devtools-select-menu
        id="device-scope-select"
        class="field-data-option"
        @selectmenuselected=${this.#Pt}
        .showDivider=${!0}
        .showArrow=${!0}
        .sideButton=${!1}
        .showSelectedItem=${!0}
        .showConnector=${!1}
        .buttonTitle=${st(ot.device,{PH1:t})}
        .disabled=${e}
        title=${st(ot.showFieldDataForDevice,{PH1:t})}
      >
        ${nt.map((e=>Ze`
            <devtools-menu-item
              .value=${e}
              .selected=${this.#Ge===e}
            >
              ${this.#Ct(e)}
            </devtools-menu-item>
          `))}
      </devtools-select-menu>
    `}#Tt(){const e=this.#at();if(!e)return null;const{firstDate:t,lastDate:i}=e.record.collectionPeriod,n=new Date(t.year,t.month-1,t.day),o=new Date(i.year,i.month-1,i.day),r={year:"numeric",month:"short",day:"numeric"};return st(ot.dateRange,{PH1:n.toLocaleDateString(void 0,r),PH2:o.toLocaleDateString(void 0,r)})}#Lt(){const e=this.#Tt(),i=document.createElement("span");i.classList.add("collection-period-range"),i.textContent=e||st(ot.notEnoughData);const n=t.i18n.getFormatLocalizedString(rt,ot.collectionPeriod,{PH1:i});return Ze`
      <div class="field-data-message">${n}</div>
    `}#Rt(){if(c.CrUXManager.instance().getConfigSetting().get().enabled)return this.#Lt();const e=o.XLink.XLink.create("https://developer.chrome.com/docs/crux",t.i18n.lockedString("Chrome UX Report")),i=t.i18n.getFormatLocalizedString(rt,ot.seeHowYourLocalMetricsCompare,{PH1:e});return Ze`
      <div class="field-data-message">${i}</div>
    `}#It(){return Ze`
      <section class="logs-section" aria-label=${st(ot.eventLogs)}>
        <devtools-live-metrics-logs
          on-render=${n.Directives.nodeRenderedCallback((e=>{this.#Je=e}))}
        >
          ${this.#Et()}
          ${this.#Mt()}
        </devtools-live-metrics-logs>
      </section>
    `}async#mt(e){const t=this.#e.getElementById(e.interactionId);if(!t||!this.#Je)return;this.#Je.selectTab("interactions")&&await it.write((()=>{t.scrollIntoView({block:"center"}),t.focus(),o.UIUtils.runCSSAnimationOnce(t,"highlight")}))}async#Dt(e){await w.LiveMetrics.instance().logInteractionScripts(e)&&await v.Console.Console.instance().showPromise()}#Et(){return this.#Ve.size?Ze`
      <ol class="log"
        slot="interactions-log-content"
        on-render=${n.Directives.nodeRenderedCallback((e=>{this.#Qe=e}))}
      >
        ${this.#Ve.values().map((e=>{const i=_e("timeline.landing.interaction-event-timing",e.duration,ze,(e=>t.TimeUtilities.preciseMillisToString(e)),{dim:!0}),n=this.#qe&&this.#qe.value<e.duration,o=this.#qe?.interactionId===e.interactionId;return Ze`
            <li id=${e.interactionId} class="log-item interaction" tabindex="-1">
              <details>
                <summary>
                  <span class="interaction-type">
                    ${e.interactionType}
                    ${o?Ze`<span class="interaction-inp-chip" title=${st(ot.inpInteraction)}>INP</span>`:Qe}
                  </span>
                  <span class="interaction-node">${e.node&&tt(v.Linkifier.Linkifier.linkify(e.node))}</span>
                  ${n?Ze`<devtools-icon
                    class="interaction-info"
                    name="info"
                    title=${st(ot.interactionExcluded)}
                  ></devtools-icon>`:Qe}
                  <span class="interaction-duration">${i}</span>
                </summary>
                <div class="phase-table" role="table">
                  <div class="phase-table-row phase-table-header-row" role="row">
                    <div role="columnheader">${st(ot.phase)}</div>
                    <div role="columnheader">
                      ${e.longAnimationFrameTimings.length?Ze`
                        <button
                          class="log-extra-details-button"
                          title=${st(ot.logToConsole)}
                          @click=${()=>this.#Dt(e)}
                        >${st(ot.duration)}</button>
                      `:st(ot.duration)}
                    </div>
                  </div>
                  <div class="phase-table-row" role="row">
                    <div role="cell">${st(ot.inputDelay)}</div>
                    <div role="cell">${Math.round(e.phases.inputDelay)}</div>
                  </div>
                  <div class="phase-table-row" role="row">
                    <div role="cell">${st(ot.processingDuration)}</div>
                    <div role="cell">${Math.round(e.phases.processingDuration)}</div>
                  </div>
                  <div class="phase-table-row" role="row">
                    <div role="cell">${st(ot.presentationDelay)}</div>
                    <div role="cell">${Math.round(e.phases.presentationDelay)}</div>
                  </div>
                </div>
              </details>
            </li>
          `}))}
      </ol>
    `:r.nothing}async#gt(e){if(!this.#Je)return;const t=[];for(const i of e){const e=this.#e.getElementById(i);e&&t.push(e)}if(!t.length)return;this.#Je.selectTab("layout-shifts")&&await it.write((()=>{t[0].scrollIntoView({block:"start"}),t[0].focus();for(const e of t)o.UIUtils.runCSSAnimationOnce(e,"highlight")}))}#Mt(){return this.#We.length?Ze`
      <ol class="log"
        slot="layout-shifts-log-content"
        on-render=${n.Directives.nodeRenderedCallback((e=>{this.#et=e}))}
      >
        ${this.#We.map((e=>{const t=_e("timeline.landing.layout-shift-event-score",e.score,Ae,(e=>e.toFixed(4)),{dim:!0});return Ze`
            <li id=${e.uniqueLayoutShiftId} class="log-item layout-shift" tabindex="-1">
              <div class="layout-shift-score">Layout shift score: ${t}</div>
              <div class="layout-shift-nodes">
                ${e.affectedNodes.map((({node:e})=>Ze`
                  <div class="layout-shift-node">${tt(v.Linkifier.Linkifier.linkify(e))}</div>
                `))}
              </div>
            </li>
          `}))}
      </ol>
    `:r.nothing}#i=()=>{const e=c.CrUXManager.instance().getConfigSetting().get().enabled,t=st(e?ot.localAndFieldMetrics:ot.localMetrics),i=Ze`
      <div class="container">
        <div class="live-metrics-view">
          <main class="live-metrics">
            <h2 class="section-title">${t}</h2>
            <div class="metric-cards"
              on-render=${n.Directives.nodeRenderedCallback((e=>{this.#Ze=e}))}
            >
              <div id="lcp">
                ${this.#ht()}
              </div>
              <div id="cls">
                ${this.#ut()}
              </div>
              <div id="inp">
                ${this.#pt()}
              </div>
            </div>
            <x-link
              href=${"https://web.dev/articles/lab-and-field-data-differences#lab_data_versus_field_data"}
              class="local-field-link"
              title=${st(ot.localFieldLearnMoreTooltip)}
            >${st(ot.localFieldLearnMoreLink)}</x-link>
            ${this.#It()}
          </main>
          <aside class="next-steps" aria-labelledby="next-steps-section-title">
            <h2 id="next-steps-section-title" class="section-title">${st(ot.nextSteps)}</h2>
            <div id="field-setup" class="settings-card">
              <h3 class="card-title">${st(ot.fieldData)}</h3>
              ${this.#Rt()}
              ${this.#kt()}
              ${this.#$t()}
              <div class="field-setup-buttons">
                <devtools-field-settings-dialog></devtools-field-settings-dialog>
              </div>
            </div>
            <div id="recording-settings" class="settings-card">
              ${this.#yt()}
            </div>
            <div id="record" class="record-action-card">
              ${this.#vt(this.#Ye)}
            </div>
            <div id="record-page-load" class="record-action-card">
              ${this.#vt(this.#Xe)}
            </div>
          </aside>
        </div>
      </div>
    `;r.render(i,this.#e,{host:this})}}class lt extends o.Widget.WidgetElement{#Ht;constructor(){super(),this.style.display="contents"}selectTab(e){return!!this.#Ht&&this.#Ht.selectTab(e)}#Ft(){const e=w.LiveMetrics.instance();switch(this.#Ht?.selectedTabId){case"interactions":e.clearInteractions();break;case"layout-shifts":e.clearLayoutShifts()}}createWidget(){const e=new o.Widget.Widget(!0,void 0,this);e.contentElement.style.display="contents",this.#Ht=new o.TabbedPane.TabbedPane;const t=document.createElement("slot");t.name="interactions-log-content";const i=o.Widget.Widget.getOrCreateWidget(t);this.#Ht.appendTab("interactions",st(ot.interactions),i,void 0,void 0,void 0,void 0,void 0,"timeline.landing.interactions-log");const n=document.createElement("slot");n.name="layout-shifts-log-content";const r=o.Widget.Widget.getOrCreateWidget(n);this.#Ht.appendTab("layout-shifts",st(ot.layoutShifts),r,void 0,void 0,void 0,void 0,void 0,"timeline.landing.layout-shifts-log");const s=new o.Toolbar.ToolbarButton(st(ot.clearCurrentLog),"clear",void 0,"timeline.landing.clear-log");return s.addEventListener("Click",this.#Ft,this),this.#Ht.rightToolbar().appendToolbarItem(s),this.#Ht.show(e.contentElement),e}}customElements.define("devtools-live-metrics-view",at),customElements.define("devtools-live-metrics-logs",lt);var dt=Object.freeze({__proto__:null,LiveMetricsView:at});const ct=new CSSStyleSheet;ct.replaceSync(".network-request-details-title{font-size:13px;padding:8px;display:flex;align-items:center}.network-request-details-title > div{box-sizing:border-box;width:12px;height:12px;border:1px solid var(--sys-color-divider);display:inline-block;margin-right:4px}.network-request-details-cols{display:flex}:host{border-bottom:1px solid var(--sys-color-divider);display:block;padding-bottom:5px}.network-request-details-col{flex:1}.network-request-details-row{padding:0 10px;min-height:20px}.title{color:var(--sys-color-token-subtle);overflow:hidden;padding-right:10px;display:inline-block;vertical-align:top}.value{display:inline-block;user-select:text;text-overflow:ellipsis;overflow:hidden;padding:0 3px}.devtools-link,\n.timeline-link{color:var(--text-link);text-decoration:underline;outline-offset:2px;padding:0;text-align:left;.elements-disclosure &{color:var(--text-link)}devtools-icon{vertical-align:baseline;color:var(--sys-color-primary)}:focus .selected & devtools-icon{color:var(--sys-color-tonal-container)}&:focus-visible{outline-width:unset}&.invalid-link{color:var(--text-disabled);text-decoration:none}&:not(.devtools-link-prevent-click, .invalid-link){cursor:pointer}@media (forced-colors: active){&:not(.devtools-link-prevent-click){forced-color-adjust:none;color:linktext}&:focus-visible{background:Highlight;color:HighlightText}}}.text-button.link-style,\n.text-button.link-style:hover,\n.text-button.link-style:active{background:none;border:none;font:inherit}.timing-rows{width:fit-content}\n/*# sourceURL=networkRequestDetails.css */\n");const ht=new CSSStyleSheet;ht.replaceSync(".bold{font-weight:bold}.url{margin-left:15px;margin-right:5px}.url--host{color:var(--sys-color-token-subtle)}.priority-row{margin-left:15px}.network-category-chip{box-sizing:border-box;width:10px;height:10px;border:1px solid var(--sys-color-divider);display:inline-block;margin-right:4px}devtools-icon.priority{height:13px;width:13px;color:var(--sys-color-on-surface-subtle)}.render-blocking{margin-left:15px;color:var(--sys-color-error)}.divider{border-top:1px solid var(--sys-color-divider);margin:5px 0}.timings-row{align-self:start;display:flex;align-items:center;width:100%}.indicator{display:inline-block;width:10px;height:4px;margin-right:5px;border:1px solid var(--sys-color-on-surface-subtle)}.whisker-left{align-self:center;display:inline-flex;width:10px;height:6px;margin-right:5px;border-left:1px solid var(--sys-color-on-surface-subtle)}.whisker-right{align-self:center;display:inline-flex;width:10px;height:6px;margin-right:5px;border-right:1px solid var(--sys-color-on-surface-subtle)}.horizontal{background-color:var(--sys-color-on-surface-subtle);height:1px;width:10px;align-self:center}.time{margin-left:auto;display:inline-block;padding-left:10px}.timings-row--duration{.indicator{border-color:transparent}.time{font-weight:var(--ref-typeface-weight-medium)}}\n/*# sourceURL=networkRequestTooltip.css */\n");const{html:ut}=r,gt={priority:"Priority",duration:"Duration",queuingAndConnecting:"Queuing and connecting",requestSentAndWaiting:"Request sent and waiting",contentDownloading:"Content downloading",waitingOnMainThread:"Waiting on main thread",renderBlocking:"Render blocking"},pt=t.i18n.registerUIStrings("panels/timeline/components/NetworkRequestTooltip.ts",gt),mt=t.i18n.getLocalizedString.bind(void 0,pt);class vt extends HTMLElement{#e=this.attachShadow({mode:"open"});#Ot;connectedCallback(){this.#e.adoptedStyleSheets=[ht],this.#i()}set networkRequest(e){this.#Ot!==e&&(this.#Ot=e,this.#i())}static renderPriorityValue(e){return e.args.data.priority===e.args.data.initialPriority?ut`${x.NetworkPriorities.uiLabelForNetworkPriority(e.args.data.priority)}`:ut`${x.NetworkPriorities.uiLabelForNetworkPriority(e.args.data.initialPriority)}
        <devtools-icon name=${"arrow-forward"} class="priority"></devtools-icon>
        ${x.NetworkPriorities.uiLabelForNetworkPriority(e.args.data.priority)}`}static renderTimings(e){const i=e.args.data.syntheticData,n=i.sendStartTime-e.ts,o=i.downloadStart-i.sendStartTime,s=i.finishTime-i.downloadStart,a=e.ts+e.dur-i.finishTime,l=Oe(e),d={backgroundColor:`color-mix(in srgb, ${l}, hsla(0, 100%, 100%, 0.8))`},c={backgroundColor:l},h=ut`<span class="whisker-left"> <span class="horizontal"></span> </span>`,u=ut`<span class="whisker-right"> <span class="horizontal"></span> </span>`;return ut`
      <div class="timings-row timings-row--duration">
        <span class="indicator"></span>
        ${mt(gt.duration)}
         <span class="time">${t.TimeUtilities.formatMicroSecondsTime(e.dur)}</span>
      </div>
      <div class="timings-row">
        ${h}
        ${mt(gt.queuingAndConnecting)}
        <span class="time">${t.TimeUtilities.formatMicroSecondsTime(n)}</span>
      </div>
      <div class="timings-row">
        <span class="indicator" style=${r.Directives.styleMap(d)}></span>
        ${mt(gt.requestSentAndWaiting)}
        <span class="time">${t.TimeUtilities.formatMicroSecondsTime(o)}</span>
      </div>
      <div class="timings-row">
        <span class="indicator" style=${r.Directives.styleMap(c)}></span>
        ${mt(gt.contentDownloading)}
        <span class="time">${t.TimeUtilities.formatMicroSecondsTime(s)}</span>
      </div>
      <div class="timings-row">
        ${u}
        ${mt(gt.waitingOnMainThread)}
        <span class="time">${t.TimeUtilities.formatMicroSecondsTime(a)}</span>
      </div>
    `}#i(){if(!this.#Ot)return;const e={backgroundColor:`${Oe(this.#Ot)}`},t=new URL(this.#Ot.args.data.url),n=ut`
      <div class="performance-card">
        <div class="url">${d.StringUtilities.trimMiddle(t.href.replace(t.origin,""),60)}</div>
        <div class="url url--host">${t.origin.replace("https://","")}</div>

        <div class="divider"></div>
        <div class="network-category"><span class="network-category-chip" style=${r.Directives.styleMap(e)}></span>${He(this.#Ot)}</div>
        <div class="priority-row">${mt(gt.priority)}: ${vt.renderPriorityValue(this.#Ot)}</div>
        ${i.Helpers.Network.isSyntheticNetworkRequestEventRenderBlocking(this.#Ot)?ut`<div class="render-blocking"> ${mt(gt.renderBlocking)} </div>`:r.nothing}
        <div class="divider"></div>

        ${vt.renderTimings(this.#Ot)}
      </div>
    `;r.render(n,this.#e,{host:this})}}customElements.define("devtools-performance-network-request-tooltip",vt);var bt=Object.freeze({__proto__:null,NetworkRequestTooltip:vt});const{html:ft}=r,yt={requestMethod:"Request method",priority:"Priority",encodedData:"Encoded data",decodedBody:"Decoded body",yes:"Yes",no:"No",preview:"Preview",networkRequest:"Network request",fromCache:"From cache",mimeType:"MIME type",FromMemoryCache:" (from memory cache)",FromCache:" (from cache)",FromPush:" (from push)",FromServiceWorker:" (from `service worker`)",initiatedBy:"Initiated by",blocking:"Blocking",inBodyParserBlocking:"In-body parser blocking",renderBlocking:"Render blocking"},wt=t.i18n.registerUIStrings("panels/timeline/components/NetworkRequestDetails.ts",yt),St=t.i18n.getLocalizedString.bind(void 0,wt);class kt extends HTMLElement{#e=this.attachShadow({mode:"open"});#Ot=null;#Nt=null;#At=new WeakMap;#zt;#V=null;constructor(e){super(),this.#zt=e}connectedCallback(){this.#e.adoptedStyleSheets=[ct,ht]}async setData(e,t,i){this.#Ot===t&&e===this.#V||(this.#V=e,this.#Ot=t,this.#Nt=i,await this.#i())}#j(){if(!this.#Ot)return null;const e={backgroundColor:`${Oe(this.#Ot)}`};return ft`
      <div class="network-request-details-title">
        <div style=${r.Directives.styleMap(e)}></div>
        ${St(yt.networkRequest)}
      </div>
    `}#Ut(e,t){return t?ft`
      <div class="network-request-details-row"><div class="title">${e}</div><div class="value">${t}</div></div>
    `:null}#_t(){if(!this.#Ot)return null;const e={tabStop:!0,showColumnNumber:!1,inlineFrameIndex:0,maxLength:100},t=g.Linkifier.Linkifier.linkifyURL(this.#Ot.args.data.url,e),i=p.NetworkRequest.getNetworkRequest(this.#Ot);if(i){t.addEventListener("contextmenu",(e=>{if(!this.#Ot)return;const t=new o.ContextMenu.ContextMenu(e,{useSoftMenu:!0});t.appendApplicableItems(new p.NetworkRequest.TimelineNetworkRequest(i)),t.show()}));const e=ft`
        ${t}
        <devtools-request-link-icon .data=${{request:i}}>
        </devtools-request-link-icon>
      `;return ft`<div class="network-request-details-row">${e}</div>`}return ft`<div class="network-request-details-row">${t}</div>`}#Bt(){if(!this.#Ot)return null;const e=this.#Ot.args.data.syntheticData.isMemoryCached||this.#Ot.args.data.syntheticData.isDiskCached;return this.#Ut(St(yt.fromCache),St(e?yt.yes:yt.no))}#qt(){if(!this.#Ot)return null;let e="";return this.#Ot.args.data.syntheticData.isMemoryCached?e+=St(yt.FromMemoryCache):this.#Ot.args.data.syntheticData.isDiskCached?e+=St(yt.FromCache):this.#Ot.args.data.timing?.pushStart&&(e+=St(yt.FromPush)),this.#Ot.args.data.fromServiceWorker&&(e+=St(yt.FromServiceWorker)),!this.#Ot.args.data.encodedDataLength&&e||(e=`${t.ByteUtilities.bytesToString(this.#Ot.args.data.encodedDataLength)}${e}`),this.#Ut(St(yt.encodedData),e)}#Vt(){if(!this.#Ot)return null;if(null!==i.Helpers.Trace.stackTraceForEvent(this.#Ot)){const e=i.Helpers.Trace.getZeroIndexedStackTraceForEvent(this.#Ot)?.at(0)??null;if(e){const t=this.#zt.maybeLinkifyConsoleCallFrame(this.#Nt,e,{tabStop:!0,inlineFrameIndex:0,showColumnNumber:!0});if(t)return this.#Ut(St(yt.initiatedBy),t)}}const e=this.#V?.NetworkRequests.eventToInitiator.get(this.#Ot);if(e){const t=this.#zt.maybeLinkifyScriptLocation(this.#Nt,null,e.args.data.url,void 0);if(t)return this.#Ut(St(yt.initiatedBy),t)}return null}#Wt(){if(!this.#Ot||!u.Network.isSyntheticNetworkRequestEventRenderBlocking(this.#Ot))return null;let e;switch(this.#Ot.args.data.renderBlocking){case"blocking":e=yt.renderBlocking;break;case"in_body_parser_blocking":e=yt.inBodyParserBlocking;break;default:return null}return this.#Ut(St(yt.blocking),e)}async#jt(){if(!this.#Ot)return null;if(!this.#At.get(this.#Ot)&&this.#Ot.args.data.url&&this.#Nt){const e=await g.ImagePreview.ImagePreview.build(this.#Nt,this.#Ot.args.data.url,!1,{imageAltText:g.ImagePreview.ImagePreview.defaultAltTextForImageURL(this.#Ot.args.data.url),precomputedFeatures:void 0,align:"start"});this.#At.set(this.#Ot,e)}const e=this.#At.get(this.#Ot);return e?this.#Ut(St(yt.preview),e):null}async#i(){if(!this.#Ot)return;const e=this.#Ot.args.data,i=ft`
      ${this.#j()}
      ${this.#_t()}
      <div class="network-request-details-cols">
        <div class="network-request-details-col">
          ${this.#Ut(St(yt.requestMethod),e.requestMethod)}
          ${this.#Ut(St(yt.priority),vt.renderPriorityValue(this.#Ot))}
          ${this.#Ut(St(yt.mimeType),e.mimeType)}
          ${this.#qt()}
          ${this.#Ut(St(yt.decodedBody),t.ByteUtilities.bytesToString(this.#Ot.args.data.decodedBodyLength))}
          ${this.#Wt()}
          ${this.#Bt()}
        </div>
        <div class="network-request-details-col">
          <div class="timing-rows">
            ${vt.renderTimings(this.#Ot)}
          </div>
        </div>
      </div>
      ${this.#Vt()}
      ${await this.#jt()}
    `;r.render(i,this.#e,{host:this})}}customElements.define("devtools-performance-network-request-details",kt);var xt=Object.freeze({__proto__:null,NetworkRequestDetails:kt});const Ct=new CSSStyleSheet;Ct.replaceSync(":host{display:block;padding:var(--sys-size-4)}ul{list-style:none;margin:0;padding:0;display:flex;flex-wrap:wrap;gap:var(--sys-size-5);justify-content:flex-start;align-items:center}.insight-chip button{background:none;user-select:none;font:var(--sys-typescale-body4-regular);border:var(--sys-size-1) solid var(--sys-color-primary);border-radius:var(--sys-shape-corner-extra-small);display:flex;margin-top:var(--sys-size-4);padding:var(--sys-size-2) var(--sys-size-4) var(--sys-size-2) var(--sys-size-4);width:max-content;white-space:pre;.keyword{color:var(--sys-color-primary);padding-right:var(--sys-size-3)}}.insight-chip button:hover{background-color:var(--sys-color-state-hover-on-subtle);cursor:pointer;transition:opacity 0.2s ease}\n/*# sourceURL=relatedInsightChips.css */\n");const{html:Pt}=r,$t={insightKeyword:"Insight"},Tt=t.i18n.registerUIStrings("panels/timeline/components/RelatedInsightChips.ts",$t),Lt=t.i18n.getLocalizedString.bind(void 0,Tt);class Rt extends HTMLElement{#e=this.attachShadow({mode:"open"});#t=this.#i.bind(this);#be={eventToRelatedInsightsMap:new Map,activeEvent:null};connectedCallback(){this.#e.adoptedStyleSheets=[Ct],this.#i()}set activeEvent(e){e!==this.#be.activeEvent&&(this.#be.activeEvent=e,n.ScheduledRender.scheduleRender(this,this.#t))}set eventToRelatedInsightsMap(e){this.#be.eventToRelatedInsightsMap=e,n.ScheduledRender.scheduleRender(this,this.#t)}#Gt(e){return t=>{t.preventDefault(),e.activateInsight()}}#i(){const{activeEvent:e,eventToRelatedInsightsMap:t}=this.#be,i=e?t.get(e)??[]:[];if(!e||0===t.size||0===i.length)return void r.render(Pt``,this.#e,{host:this});const n=i.map((e=>Pt`
      <li class="insight-chip">
        <button type="button" @click=${this.#Gt(e)}>
          <span class="keyword">${Lt($t.insightKeyword)}</span>
          <span class="insight-label">${e.insightLabel}</span>
        </button>
      </li>
      `));r.render(Pt`<ul>${n}</ul>`,this.#e,{host:this})}}customElements.define("devtools-related-insight-chips",Rt);var It=Object.freeze({__proto__:null,RelatedInsightChips:Rt});const Et=new CSSStyleSheet;Et.replaceSync(":host{display:block;height:100%}.annotations{display:flex;flex-direction:column;height:100%;padding:0}.visibility-setting{margin-top:auto}.annotation-container{display:flex;justify-content:space-between;align-items:center;padding:0 10px;.delete-button{visibility:hidden;border:none;background:none}&:hover,\n  &:focus-within{background-color:var(--sys-color-neutral-container);button.delete-button{visibility:visible}}}.annotation{display:flex;flex-direction:column;align-items:flex-start;word-break:break-word;padding:var(--sys-size-8) 0;gap:6px}.annotation-identifier{padding:4px 8px;border-radius:10px;font-weight:bold;&.time-range{background-color:var(--app-color-performance-sidebar-time-range);color:var(--app-color-performance-sidebar-label-text-light)}}.entries-link{display:flex;flex-wrap:wrap;row-gap:2px;align-items:center}.label{font-size:larger}.annotation-tutorial-container{padding:10px}.tutorial-card{display:block;position:relative;margin:10px 0;padding:10px;border-radius:var(--sys-shape-corner-extra-small);overflow:hidden;border:1px solid var(--sys-color-divider);background-color:var(--sys-color-base)}.tutorial-image{display:flex;justify-content:center;& > img{max-width:100%;height:auto}}.tutorial-title,\n.tutorial-description{margin:5px 0}\n/*# sourceURL=sidebarAnnotationsTab.css */\n");const{html:Mt}=r,Dt=new URL("../../../Images/performance-panel-diagram.svg",import.meta.url).toString(),Ht=new URL("../../../Images/performance-panel-entry-label.svg",import.meta.url).toString(),Ft=new URL("../../../Images/performance-panel-time-range.svg",import.meta.url).toString(),Ot=new URL("../../../Images/performance-panel-delete-annotation.svg",import.meta.url).toString(),Nt={annotationGetStarted:"Annotate a trace for yourself and others",entryLabelTutorialTitle:"Label an item",entryLabelTutorialDescription:"Double-click on an item and type to create an item label.",entryLinkTutorialTitle:"Connect two items",entryLinkTutorialDescription:"Double-click on an item, click on the adjacent rightward arrow, then select the destination item.",timeRangeTutorialTitle:"Define a time range",timeRangeTutorialDescription:"Shift-drag in the flamechart then type to create a time range annotation.",deleteAnnotationTutorialTitle:"Delete an annotation",deleteAnnotationTutorialDescription:"Hover over the list in the sidebar with Annotations tab selected to access the delete function.",deleteButton:"Delete annotation: {PH1}",entryLabelDescriptionLabel:'A "{PH1}" event annotated with the text "{PH2}"',timeRangeDescriptionLabel:"A time range starting at {PH1} and ending at {PH2}",entryLinkDescriptionLabel:'A link between a "{PH1}" event and a "{PH2}" event'},At=t.i18n.registerUIStrings("panels/timeline/components/SidebarAnnotationsTab.ts",Nt),zt=t.i18n.getLocalizedString.bind(void 0,At);class Ut extends HTMLElement{#e=this.attachShadow({mode:"open"});#t=this.#i.bind(this);#Kt=[];#Yt=new Map;#Xt;constructor(){super(),this.#Xt=v.Settings.Settings.instance().moduleSetting("annotations-hidden")}set annotations(e){this.#Kt=this.#Jt(e),n.ScheduledRender.scheduleRender(this,this.#t)}set annotationEntryToColorMap(e){this.#Yt=e}#Jt(e){const t=new Set,i=e.filter((e=>{if(this.#Zt(e))return!0;if("ENTRIES_LINK"===e.type||"ENTRY_LABEL"===e.type){const i="ENTRIES_LINK"===e.type?e.entryFrom:e.entry;if(t.has(i))return!1;t.add(i)}return!0}));return i.sort(((e,t)=>this.#Qt(e)-this.#Qt(t))),i}#Qt(e){switch(e.type){case"ENTRY_LABEL":return e.entry.ts;case"ENTRIES_LINK":return e.entryFrom.ts;case"TIME_RANGE":return e.bounds.min;default:d.assertNever(e,`Invalid annotation type ${e}`)}}#Zt(e){switch(e.type){case"ENTRY_LABEL":return e.label.length>0;case"ENTRIES_LINK":return Boolean(e.entryTo);case"TIME_RANGE":return e.bounds.range>0}}connectedCallback(){this.#e.adoptedStyleSheets=[Et],n.ScheduledRender.scheduleRender(this,this.#t)}#ei(e){if(e.entryTo){const t=p.EntryName.nameForEntry(e.entryTo),i=this.#Yt.get(e.entryTo)??"",n={backgroundColor:i,color:_t(i)};return Mt`
        <span class="annotation-identifier" style=${r.Directives.styleMap(n)}>
          ${t}
        </span>`}return r.nothing}#ti(t){switch(t.type){case"ENTRY_LABEL":{const e=p.EntryName.nameForEntry(t.entry),i=this.#Yt.get(t.entry)??"",n={backgroundColor:i,color:_t(i)};return Mt`
              <span class="annotation-identifier" style=${r.Directives.styleMap(n)}>
                ${e}
              </span>
        `}case"TIME_RANGE":{const n=e.TraceBounds.BoundsManager.instance().state()?.milli.entireTraceBounds.min??0,o=Math.round(i.Helpers.Timing.microSecondsToMilliseconds(t.bounds.min)-n),r=Math.round(i.Helpers.Timing.microSecondsToMilliseconds(t.bounds.max)-n);return Mt`
              <span class="annotation-identifier time-range">
                ${o} - ${r} ms
              </span>
        `}case"ENTRIES_LINK":{const e=p.EntryName.nameForEntry(t.entryFrom),i=this.#Yt.get(t.entryFrom)??"",n={backgroundColor:i,color:_t(i)};return Mt`
          <div class="entries-link">
            <span class="annotation-identifier" style=${r.Directives.styleMap(n)}>
              ${e}
            </span>
            <devtools-icon class="inline-icon" .data=${{iconName:"arrow-forward",color:"var(--icon-default)",width:"18px",height:"18px"}}>
            </devtools-icon>
            ${this.#ei(t)}
          </div>
      `}default:d.assertNever(t,"Unsupported annotation type")}}#ii(e){this.dispatchEvent(new si(e))}#ni(){return Mt`
      <div class="annotation-tutorial-container">
      ${zt(Nt.annotationGetStarted)}
        <div class="tutorial-card">
          <div class="tutorial-image"> <img src=${Ht}></img></div>
          <div class="tutorial-title">${zt(Nt.entryLabelTutorialTitle)}</div>
          <div class="tutorial-description">${zt(Nt.entryLabelTutorialDescription)}</div>
        </div>
        <div class="tutorial-card">
          <div class="tutorial-image"> <img src=${Dt}></img></div>
          <div class="tutorial-title">${zt(Nt.entryLinkTutorialTitle)}</div>
          <div class="tutorial-description">${zt(Nt.entryLinkTutorialDescription)}</div>
        </div>
        <div class="tutorial-card">
          <div class="tutorial-image"> <img src=${Ft}></img></div>
          <div class="tutorial-title">${zt(Nt.timeRangeTutorialTitle)}</div>
          <div class="tutorial-description">${zt(Nt.timeRangeTutorialDescription)}</div>
        </div>
        <div class="tutorial-card">
          <div class="tutorial-image"> <img src=${Ot}></img></div>
          <div class="tutorial-title">${zt(Nt.deleteAnnotationTutorialTitle)}</div>
          <div class="tutorial-description">${zt(Nt.deleteAnnotationTutorialDescription)}</div>
        </div>
      </div>
    `}#oi(e){switch(e.type){case"ENTRY_LABEL":return"entry-label";case"TIME_RANGE":return"time-range";case"ENTRIES_LINK":return"entries-link";default:d.assertNever(e,"unknown annotation type")}}#i(){r.render(Mt`
        <span class="annotations">
          ${0===this.#Kt.length?this.#ni():Mt`
              ${this.#Kt.map((e=>{const i=function(e){switch(e.type){case"ENTRY_LABEL":{const t=p.EntryName.nameForEntry(e.entry);return zt(Nt.entryLabelDescriptionLabel,{PH1:t,PH2:e.label})}case"TIME_RANGE":{const i=t.TimeUtilities.formatMicroSecondsAsMillisFixedExpanded(e.bounds.min),n=t.TimeUtilities.formatMicroSecondsAsMillisFixedExpanded(e.bounds.max);return zt(Nt.timeRangeDescriptionLabel,{PH1:i,PH2:n})}case"ENTRIES_LINK":{if(!e.entryTo)return"";const t=p.EntryName.nameForEntry(e.entryFrom),i=p.EntryName.nameForEntry(e.entryTo);return zt(Nt.entryLinkDescriptionLabel,{PH1:t,PH2:i})}default:d.assertNever(e,"Unsupported annotation")}}(e);return Mt`
                  <div class="annotation-container"
                    @click=${()=>this.#ii(e)}
                    aria-label=${i}
                    tabindex="0"
                    jslog=${s.item(`timeline.annotation-sidebar.annotation-${this.#oi(e)}`).track({click:!0})}
                  >
                    <div class="annotation">
                      ${this.#ti(e)}
                      <span class="label">
                        ${"ENTRY_LABEL"===e.type||"TIME_RANGE"===e.type?e.label:""}
                      </span>
                    </div>
                    <button class="delete-button" aria-label=${zt(Nt.deleteButton,{PH1:i})} @click=${t=>{t.stopPropagation(),this.dispatchEvent(new ri(e))}} jslog=${s.action("timeline.annotation-sidebar.delete").track({click:!0})}>
                      <devtools-icon
                        class="bin-icon"
                        .data=${{iconName:"bin",color:"var(--icon-default)",width:"20px",height:"20px"}}
                      ></devtools-icon>
                    </button>
                  </div>`}))}
              <setting-checkbox class="visibility-setting" .data=${{setting:this.#Xt,textOverride:"Hide annotations"}}>
              </setting-checkbox>`}
      </span>`,this.#e,{host:this})}}function _t(e){const t=v.Color.parse(e)?.asLegacyColor(),i="--app-color-performance-sidebar-label-text-dark",n=v.Color.parse(f.ThemeSupport.instance().getComputedValue(i))?.asLegacyColor();if(!t||!n)return`var(${i})`;return v.ColorUtils.contrastRatio(t.rgba(),n.rgba())>=4.5?`var(${i})`:"var(--app-color-performance-sidebar-label-text-light)"}customElements.define("devtools-performance-sidebar-annotations",Ut);var Bt=Object.freeze({__proto__:null,SidebarAnnotationsTab:Ut});const qt=new CSSStyleSheet;qt.replaceSync(":host{display:block;padding:5px 10px}.metrics-row{display:flex;flex-direction:row}.metric{flex:1;user-select:text;cursor:pointer;background:none;border:none;padding:0;display:block;text-align:left}.metric-value{font-size:var(--sys-size-11)}.metric-value-bad{color:var(--app-color-performance-bad)}.metric-value-ok{color:var(--app-color-performance-ok)}.metric-value-good{color:var(--app-color-performance-good)}.metric-score-unclassified{color:var(--sys-color-token-subtle)}.metric-label{font:var(--sys-typescale-body4-medium)}.number-with-unit{white-space:nowrap;.unit{font-size:14px;padding:0 1px}}\n/*# sourceURL=sidebarSingleInsightSet.css */\n");const{html:Vt}=r,Wt={metricScore:"{PH1}: {PH2} {PH3} score"},jt=t.i18n.registerUIStrings("panels/timeline/components/SidebarSingleInsightSet.ts",Wt),Gt=t.i18n.getLocalizedString.bind(void 0,jt),Kt=new Set(["FontDisplay"]),Yt={InteractionToNextPaint:m.InteractionToNextPaint.InteractionToNextPaint,LCPPhases:m.LCPPhases.LCPPhases,LCPDiscovery:m.LCPDiscovery.LCPDiscovery,CLSCulprits:m.CLSCulprits.CLSCulprits,RenderBlocking:m.RenderBlocking.RenderBlocking,DocumentLatency:m.DocumentLatency.DocumentLatency,FontDisplay:m.FontDisplay.FontDisplay,Viewport:m.Viewport.Viewport,ThirdParties:m.ThirdParties.ThirdParties,SlowCSSSelector:m.SlowCSSSelector.SlowCSSSelector};class Xt extends HTMLElement{#e=this.attachShadow({mode:"open"});#ri=this.#i.bind(this);#be={parsedTrace:null,insights:null,insightSetKey:null,activeCategory:m.Types.Category.ALL,activeInsight:null};set data(e){this.#be=e,n.ScheduledRender.scheduleRender(this,this.#ri)}connectedCallback(){this.#e.adoptedStyleSheets=[qt],this.#i()}#si(e){return this.#be.activeCategory===m.Types.Category.ALL||e===this.#be.activeCategory}#ai(e){this.dispatchEvent(new m.EventRef.EventReferenceClick(e))}#li(e,t,i,n){const o="string"==typeof t?t:t.text,s="string"==typeof t?t:t.element,a=Gt(Wt.metricScore,{PH1:e,PH2:o,PH3:i});return this.#si(e)?Vt`
      <button class="metric"
        @click=${n?this.#ai.bind(this,n):null}
        title=${a}
        aria-label=${a}
      >
        <div class="metric-value metric-value-${i}">${s}</div>
        <div class="metric-label">${e}</div>
      </button>
    `:r.nothing}#di(e){const t=i.Insights.Common.getInsight("InteractionToNextPaint",this.#be.insights,e);if(!t?.longestInteractionEvent?.dur)return null;return{value:t.longestInteractionEvent.dur,event:t.longestInteractionEvent}}#ci(e){const t=i.Insights.Common.getInsight("LCPPhases",this.#be.insights,e);if(!t||!t.lcpMs||!t.lcpEvent)return null;return{value:i.Helpers.Timing.millisecondsToMicroseconds(t.lcpMs),event:t.lcpEvent}}#hi(e){const t=i.Insights.Common.getInsight("CLSCulprits",this.#be.insights,e);if(!t)return{value:0,worstShiftEvent:null};let n,o=0;for(const e of t.clusters)e.clusterCumulativeScore>o&&(o=e.clusterCumulativeScore,n=e);return{value:o,worstShiftEvent:n?.worstShiftEvent??null}}#ui(e){const t=this.#ci(e),n=this.#hi(e),o=this.#di(e);return Vt`
    <div class="metrics-row">
    ${t?this.#li("LCP",Be.formatMicroSecondsAsSeconds(t.value),i.Handlers.ModelHandlers.PageLoadMetrics.scoreClassificationForLargestContentfulPaint(t.value),t.event??null):r.nothing}
    ${o?this.#li("INP",Be.formatMicroSecondsAsMillisFixed(o.value),i.Handlers.ModelHandlers.UserInteractions.scoreClassificationForInteractionToNextPaint(o.value),o.event):r.nothing}
    ${this.#li("CLS",n.value?n.value.toFixed(2):"0",i.Handlers.ModelHandlers.LayoutShifts.scoreClassificationForLayoutShift(n.value),n.worstShiftEvent)}
    </div>
    `}#gi(e,t,i){const n=C.Runtime.experiments.isEnabled("timeline-experimental-insights"),o=e?.get(i)?.model;if(!o)return Vt``;const r=[];for(const[e,s]of Object.entries(Yt)){if(!n&&Kt.has(e))continue;const a=o[e],l=Vt`<div data-single-insight-wrapper>
        <${s.litTagName}
          .selected=${this.#be.activeInsight?.model===a}
          .model=${a}
          .parsedTrace=${t}
          .insightSetKey=${i}
          .activeCategory=${this.#be.activeCategory}>
        </${s.litTagName}>
      </div>`;r.push(l)}return Vt`${r}`}#i(){const{parsedTrace:e,insights:t,insightSetKey:i}=this.#be;e&&t&&i?r.render(Vt`
      <div class="navigation">
        ${this.#ui(i)}
        ${this.#gi(t,e,i)}
        </div>
      `,this.#e,{host:this}):r.render(Vt``,this.#e,{host:this})}}customElements.define("devtools-performance-sidebar-single-navigation",Xt);var Jt=Object.freeze({__proto__:null,SidebarSingleInsightSet:Xt});const Zt=new CSSStyleSheet;Zt.replaceSync(":host{display:flex;flex-flow:column nowrap;flex-grow:1}.insight-sets-wrapper{display:flex;flex-flow:column nowrap;flex-grow:1;details{flex-grow:0}details[open]{flex-grow:1;border-bottom:1px solid var(--sys-color-divider)}summary{background-color:var(--sys-color-surface2);border-bottom:1px solid var(--sys-color-divider);overflow:hidden;padding:2px 5px;text-overflow:ellipsis;white-space:nowrap;font:var(--sys-typescale-body4-medium);display:flex;align-items:center;&:focus{background-color:var(--sys-color-tonal-container)}&::marker{color:var(--sys-color-on-surface-subtle);font-size:11px;line-height:1}details:first-child &{border-top:1px solid var(--sys-color-divider)}}}.zoom-button{margin-left:auto}.zoom-icon{visibility:hidden;&.active devtools-button{visibility:visible}}.dropdown-icon{&.active devtools-button{transform:rotate(90deg)}}.feedback-wrapper{position:relative;padding:var(--sys-size-6);.tooltip{visibility:hidden;transition-property:visibility;position:absolute;bottom:35px;width:90%;max-width:300px;left:var(--sys-size-6);z-index:1;box-sizing:border-box;padding:var(--sys-size-5) var(--sys-size-6);border-radius:var(--sys-shape-corner-small);background-color:var(--sys-color-cdt-base-container);box-shadow:var(--drop-shadow-depth-3)}devtools-button:hover + .tooltip{visibility:visible}}\n/*# sourceURL=sidebarInsightsTab.css */\n");const{html:Qt}=r,ei={feedbackButton:"Feedback",feedbackTooltip:"Insights is an experimental feature. Your feedback will help us improve it."},ti=t.i18n.registerUIStrings("panels/timeline/components/SidebarInsightsTab.ts",ei),ii=t.i18n.getLocalizedString.bind(void 0,ti);class ni extends HTMLElement{#t=this.#i.bind(this);#e=this.attachShadow({mode:"open"});#V=null;#pi=null;#mi=null;#vi=m.Types.Category.ALL;#bi=null;connectedCallback(){this.#e.adoptedStyleSheets=[Zt]}set parsedTrace(e){e!==this.#V&&(this.#V=e,this.#bi=null,n.ScheduledRender.scheduleRender(this,this.#t))}set insights(e){if(e===this.#pi)return;if(this.#pi=e,this.#bi=null,!this.#pi||!this.#V)return;const t=i.Helpers.Timing.millisecondsToMicroseconds(i.Types.Timing.MilliSeconds(5e3)),o=[...this.#pi.values()];this.#bi=o.find((e=>e.navigation||e.bounds.range>t))?.id??o[0]?.id??null,n.ScheduledRender.scheduleRender(this,this.#t)}set activeInsight(e){e!==this.#mi&&(this.#mi=e,this.#mi&&(this.#bi=this.#mi.insightSetKey),n.ScheduledRender.scheduleRender(this,this.#t))}#fi(e){this.#bi=this.#bi===e?null:e,this.#bi!==this.#mi?.insightSetKey&&this.dispatchEvent(new m.SidebarInsight.InsightDeactivated),n.ScheduledRender.scheduleRender(this,this.#t)}#yi(e){const t=this.#pi?.get(e);t&&this.dispatchEvent(new m.SidebarInsight.InsightSetHovered(t.bounds))}#wi(){this.dispatchEvent(new m.SidebarInsight.InsightSetHovered)}#Si(){b.InspectorFrontendHost.InspectorFrontendHostInstance.openInNewTab("https://crbug.com/371170842")}#ki(e,t){e.stopPropagation();const i=this.#pi?.get(t);i&&this.dispatchEvent(new m.SidebarInsight.InsightSetZoom(i.bounds))}#xi(e){const t=r.Directives.classMap({"zoom-icon":!0,active:e});return Qt`
    <div class=${t}>
        <devtools-button .data=${{variant:"icon",iconName:"center-focus-weak",size:"SMALL"}}
      ></devtools-button></div>`}#Ci(e){const t=r.Directives.classMap({"dropdown-icon":!0,active:e});return Qt`
      <div class=${t}>
        <devtools-button .data=${{variant:"icon",iconName:"chevron-right",size:"SMALL"}}
      ></devtools-button></div>
    `}#i(){if(!this.#V||!this.#pi)return void r.render(r.nothing,this.#e,{host:this});const e=this.#pi.size>1,t=p.Helpers.createUrlLabels([...this.#pi.values()].map((({url:e})=>e))),i=Qt`
      <div class="insight-sets-wrapper">
        ${[...this.#pi.values()].map((({id:i,url:n},o)=>{const r={parsedTrace:this.#V,insights:this.#pi,insightSetKey:i,activeCategory:this.#vi,activeInsight:this.#mi},s=Qt`
            <devtools-performance-sidebar-single-navigation
              .data=${r}>
            </devtools-performance-sidebar-single-navigation>
          `;return e?Qt`<details
              ?open=${i===this.#bi}
            >
              <summary
                @click=${()=>this.#fi(i)}
                @mouseenter=${()=>this.#yi(i)}
                @mouseleave=${()=>this.#wi()}
                title=${n.href}>
                ${this.#Ci(i===this.#bi)}
                <span>${t[o]}</span>
                <span class='zoom-button' @click=${e=>this.#ki(e,i)}>${this.#xi(i===this.#bi)}</span>
              </summary>
              ${s}
            </details>`:s}))}
      </div>

      <div class="feedback-wrapper">
        <devtools-button .variant=${"outlined"} .iconName=${"experiment"} @click=${this.#Si}>
          ${ii(ei.feedbackButton)}
        </devtools-button>

        <p class="tooltip">${ii(ei.feedbackTooltip)}</p>
      </div>
    `,n=r.Directives.repeat([i],(()=>this.#V),(e=>e));r.render(n,this.#e,{host:this})}}customElements.define("devtools-performance-sidebar-insights",ni);var oi=Object.freeze({__proto__:null,SidebarInsightsTab:ni});class ri extends Event{removedAnnotation;static eventName="removeannotation";constructor(e){super(ri.eventName,{bubbles:!0,composed:!0}),this.removedAnnotation=e}}class si extends Event{annotation;static eventName="revealannotation";constructor(e){super(si.eventName,{bubbles:!0,composed:!0}),this.annotation=e}}class ai extends o.Widget.VBox{#Ht=new o.TabbedPane.TabbedPane;#Pi=new li;#$i=new di;#Ti=0;#Li=v.Settings.Settings.instance().createSetting("timeline-user-has-opened-sidebar-once",!1);userHasOpenedSidebarOnce(){return this.#Li.get()}constructor(){super(),this.setMinimumSize(170,0),C.Runtime.experiments.isEnabled("timeline-rpp-sidebar")&&this.#Ht.appendTab("insights","Insights",this.#Pi,void 0,void 0,!1,!1,0,"timeline.insights-tab"),C.Runtime.experiments.isEnabled("perf-panel-annotations")&&this.#Ht.appendTab("annotations","Annotations",this.#$i,void 0,void 0,!1,!1,1,"timeline.annotations-tab"),this.#Ht.selectTab("insights")}wasShown(){this.#Li.set(!0),this.#Ht.show(this.element),this.#Ri(),"insights"===this.#Ht.selectedTabId&&this.#Ht.tabIsDisabled("insights")&&this.#Ht.hasTab("annotations")&&this.#Ht.selectTab("annotations")}setAnnotations(e,t){this.#$i.setAnnotations(e,t),this.#Ti=e.length,this.#Ri()}#Ri(){let e=null;if(this.#Ti>0){e=new P.Adorner.Adorner;const t=document.createElement("span");t.textContent=this.#Ti.toString(),e.data={name:"countWrapper",content:t},e.classList.add("annotations-count")}this.#Ht.setSuffixElement("annotations",e)}setParsedTrace(e){this.#Pi.setParsedTrace(e)}setInsights(e){this.#Pi.setInsights(e),this.#Ht.setTabEnabled("insights",null!==e)}setActiveInsight(e){this.#Pi.setActiveInsight(e),e&&this.#Ht.selectTab("insights")}}class li extends o.Widget.VBox{#Ii=new ni;constructor(){super(),this.element.classList.add("sidebar-insights"),this.element.appendChild(this.#Ii)}setParsedTrace(e){this.#Ii.parsedTrace=e}setInsights(e){this.#Ii.insights=e}setActiveInsight(e){this.#Ii.activeInsight=e}}class di extends o.Widget.VBox{#Ii=new Ut;constructor(){super(),this.element.classList.add("sidebar-annotations"),this.element.appendChild(this.#Ii)}setAnnotations(e,t){this.#Ii.annotationEntryToColorMap=t,this.#Ii.annotations=e}}var ci=Object.freeze({__proto__:null,DEFAULT_SIDEBAR_TAB:"insights",DEFAULT_SIDEBAR_WIDTH_PX:240,RemoveAnnotation:ri,RevealAnnotation:si,SidebarWidget:ai});export{T as Breadcrumbs,O as BreadcrumbsUI,q as CPUThrottlingSelector,G as DetailsView,ne as FieldSettingsDialog,ce as InteractionBreakdown,be as LayoutShiftDetails,dt as LiveMetricsView,Xe as MetricCard,xt as NetworkRequestDetails,bt as NetworkRequestTooltip,Pe as NetworkThrottlingSelector,It as RelatedInsightChips,ci as Sidebar,Bt as SidebarAnnotationsTab,oi as SidebarInsightsTab,Jt as SidebarSingleInsightSet,qe as Utils};
