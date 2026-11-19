// 中文：自定义动作只在有效正文中执行；菜单和手动输入暂缓计时但不关闭开关。
// English: Custom actions require a valid reader; menus and manual input defer turns without disabling the switch.
static bool rc_zoom_active(void){return s_zoom_active||s_image_zoom>READER_IMAGE_ZOOM_MIN||s_image_pan_active;}
// 中文：交互只延后计时，不关闭用户开关；仅显式关闭/离书/锁屏/书末才停止。
// English: Interactions defer turns without disabling the switch; explicit off/lifecycle/end stop it.
static void rc_pause_auto(void){if(s_rc_auto.running)s_rc_auto_waiting=true;}
static void rc_stop_auto(void){reader_auto_stop(&s_rc_auto);s_rc_auto_waiting=false;}
void app_book_pause_auto(void){rc_pause_auto();}
static void rc_notice(app_ctx_t*ctx,const char*text){copy_text(s_reader_notice,sizeof(s_reader_notice),text);s_reader_notice_until=ctx->now_ms+1800;}
static void rc_invalidate(void){
#if KIIKO_BOARD_METALIO47
    pause_reader_prep();
#else
    invalidate_prep();
#endif
}
static app_redraw_t rc_open(app_ctx_t*ctx,reader_panel_t panel){
    rc_pause_auto();s_rc_menu_slot=-1;s_rc_double_pending=false;s_reader_tap.pending=false;s_reader_slider=-1;s_pressed_control=-1;
    if(s_reader_fullscreen)(void)toggle_reader_fullscreen(ctx);
    s_reader_panel=panel;rc_invalidate();return APP_REDRAW_PAGE;
}
static app_redraw_t rc_to_root(app_ctx_t*ctx,unsigned tab){s_rc_menu_slot=-1;s_rc_tab=tab;return rc_open(ctx,READER_PANEL_REFRESH_SETTINGS);}
static app_redraw_t rc_execute(app_ctx_t*ctx,unsigned action){
    if(!s_text||s_view!=READING||s_clear_confirm||!reader_controls_action_allowed(action,KIIKO_BOARD_METALIO47))return APP_REDRAW_NONE;
    if(action!=RC_AUTOTURN)rc_pause_auto();
    s_reader_tap.pending=false;s_rc_double_pending=false;
    switch(action){
    case RC_NONE:return APP_REDRAW_NONE;
    case RC_PREVIOUS:return turn_page(ctx,-1);
    case RC_NEXT:return turn_page(ctx,1);
    case RC_BOOKMARK:{bool before=bookmark_is_current();bool ok=bookmark_toggle();rc_notice(ctx,ok?(before?"已取消书签":"已添加书签"):"书签保存失败");return APP_REDRAW_PAGE;}
    case RC_AUTOTURN:
        if(s_rc_auto.running){rc_stop_auto();rc_notice(ctx,"自动翻页已暂停");}
        else{
            if(rc_zoom_active()||s_footnote_note>=0){rc_notice(ctx,"请先返回阅读正文");return APP_REDRAW_PAGE;}
            if(s_page+1>=book_layout_page_count()&&s_chapter+1>=book_chapter_count()){rc_notice(ctx,"已到书末");return APP_REDRAW_PAGE;}
            s_reader_panel=READER_PANEL_NONE;s_rc_auto_waiting=false;reader_auto_start(&s_rc_auto,ctx->now_ms,reader_controls_seconds(app_settings_reader_controls()));rc_notice(ctx,"自动翻页已开启");
        }
        return APP_REDRAW_PAGE;
    case RC_EFFECT:{unsigned effect=(app_settings_reader_turn_effect()+1)%3;app_settings_set_reader_turn_effect(effect);s_turns=0;rc_invalidate();rc_notice(ctx,effect==0?"普通翻页":effect==1?"水波纹翻页":"速读黑白翻页");return APP_REDRAW_PAGE;}
    case RC_REFRESH:return reader_manual_refresh(ctx);
    case RC_FULLSCREEN:return toggle_reader_fullscreen(ctx);
    case RC_TOOLS:{
        app_redraw_t redraw=s_reader_fullscreen?toggle_reader_fullscreen(ctx):APP_REDRAW_PAGE;
        s_reader_panel=READER_PANEL_TOOLS;rc_invalidate();return redraw;}
    case RC_TOOLS_TOGGLE:
        if(!s_reader_fullscreen)return toggle_reader_fullscreen(ctx);
        {app_redraw_t redraw=toggle_reader_fullscreen(ctx);s_reader_panel=READER_PANEL_TOOLS;rc_invalidate();return redraw;}
    case RC_GUIDE:{unsigned style=app_settings_book_reading_line();app_settings_set_book_reading_line(style?0:1);rc_invalidate();rc_notice(ctx,style?"阅读线已关闭":"阅读线已开启");return APP_REDRAW_PAGE;}
    case RC_CONTENTS:
        save_progress();set_reader_view(TOC);s_message[0]=0;s_toc_jump_open=s_toc_jump_drag=false;s_reader_panel=READER_PANEL_NONE;ctx->leaf=book_toc_open(current_toc_position());return APP_REDRAW_PAGE;
    case RC_BOOKMARKS:
        s_bookmark_edit=s_bookmark_delete_confirm=s_bookmark_delete_error=false;s_bookmark_selected=0;s_bookmark_page=0;return rc_open(ctx,READER_PANEL_BOOKMARKS);
    case RC_PROGRESS:case RC_STATS:
        if(s_reader_fullscreen)(void)toggle_reader_fullscreen(ctx);
        reader_chapter_jump_begin();rc_invalidate();return APP_REDRAW_PAGE;
    case RC_TEXT:s_rc_font_root=true;return rc_open(ctx,READER_PANEL_FONT_SETTINGS);
    case RC_LAYOUT:return rc_open(ctx,READER_PANEL_LAYOUT_SETTINGS);
    case RC_DISPLAY:return rc_open(ctx,READER_PANEL_READING_DISPLAY);
    case RC_TURN_SETTINGS:return rc_to_root(ctx,1);
#if KIIKO_BOARD_METALIO47
    case RC_LIGHT:metalio_frontlight_toggle();rc_notice(ctx,metalio_frontlight_enabled()?"前光已开启":"前光已关闭");return APP_REDRAW_PAGE;
    case RC_BRIGHTNESS:case RC_WARMTH:s_rc_light_warm=action==RC_WARMTH;return rc_open(ctx,READER_PANEL_READING_LIGHT);
#endif
    default:return APP_REDRAW_NONE;
    }
}
static void rc_open_binding(unsigned slot,EpdRect anchor){s_rc_menu_slot=(int)slot;s_rc_menu_group=0;s_rc_anchor=anchor;}
static app_redraw_t rc_binding_tap(app_ctx_t*ctx,int x,int y){
    EpdRect r=rc_menu_rect();bool side=s_rc_menu_slot>=RC_SLOT_COUNT;
    if(!ui_rect_hit(r,x,y)){s_rc_menu_slot=-1;return APP_REDRAW_PAGE;}
    if(!side&&y<r.y+52){s_rc_menu_group=x<r.x+r.width/2?0:1;return APP_REDRAW_PAGE;}
    int index=(y-r.y-(side?6:54))/48;unsigned count=side?(KIIKO_BOARD_METALIO47?5:3):rc_pool_count(s_rc_menu_group);
    if(index<0||(unsigned)index>=count)return APP_REDRAW_NONE;
    bool ok=side?app_settings_set_reader_side(s_rc_menu_slot-RC_SLOT_COUNT,rc_side_pool(index)):
        app_settings_set_reader_control(s_rc_menu_slot,rc_pool_action(s_rc_menu_group,index));
    if(ok)s_rc_menu_slot=-1;else rc_notice(ctx,"保存失败，请重试");return APP_REDRAW_PAGE;
}
static app_redraw_t rc_panel_action(app_ctx_t*ctx,int x,int y,bool*handled){
    *handled=false;
    if(s_view!=READING||s_clear_confirm)return APP_REDRAW_NONE;
    if(s_reader_panel==READER_PANEL_READING_LIGHT){
        *handled=true;if(y<790||ui_rect_hit((EpdRect){32,815,64,64},x,y)||ui_rect_hit(rc_done_rect(),x,y))return rc_open(ctx,READER_PANEL_NONE);
        return APP_REDRAW_NONE;
    }
    if(s_reader_panel==READER_PANEL_TOUCH_SETTINGS||s_reader_panel==READER_PANEL_TOUCH_MORE){
        *handled=true;rc_pause_auto();
        if(s_rc_menu_slot>=0)return rc_binding_tap(ctx,x,y);
        if(ui_rect_hit((EpdRect){608,14,64,64},x,y))return s_reader_panel==READER_PANEL_TOUCH_MORE?rc_open(ctx,READER_PANEL_TOUCH_SETTINGS):rc_to_root(ctx,2);
        if(s_reader_panel==READER_PANEL_TOUCH_SETTINGS){
            if(ui_rect_hit((EpdRect){12,14,64,64},x,y))return rc_open(ctx,READER_PANEL_TOUCH_MORE);
            const unsigned slots[]={RC_TAP_CENTER,RC_HOLD,RC_BOTTOM_LEFT,RC_BOTTOM_CENTER,RC_BOTTOM_RIGHT,RC_SLOT_COUNT,RC_SLOT_COUNT+1};
            for(unsigned i=0;i<sizeof(slots)/sizeof(slots[0]);++i){EpdRect a=rc_touch_rect(slots[i]);if(ui_rect_hit(a,x,y)){rc_open_binding(slots[i],a);return APP_REDRAW_PAGE;}}
        }else{
            if(ui_rect_hit(rc_done_rect(),x,y))return rc_open(ctx,READER_PANEL_TOUCH_SETTINGS);
            for(unsigned i=0;i<11;++i)if(rc_more_slots[i]>=0&&ui_rect_hit(rc_more_rect(i),x,y)){rc_open_binding(rc_more_slots[i],rc_more_rect(i));return APP_REDRAW_PAGE;}
        }
        return APP_REDRAW_NONE;
    }
    if(s_reader_panel==READER_PANEL_READING_DISPLAY){
        *handled=true;if(y<740||ui_rect_hit((EpdRect){32,765,64,64},x,y)||ui_rect_hit(rc_done_rect(),x,y))return rc_to_root(ctx,0);
        for(int i=0;i<4;++i)if(ui_rect_hit(rc_display_rect(i),x,y)){
            if(!i){if(!app_settings_set_reader_default_fullscreen(!app_settings_reader_default_fullscreen()))rc_notice(ctx,"保存失败，请重试");return APP_REDRAW_PAGE;}
            return apply_reader_option(ctx,i);
        }
        return APP_REDRAW_NONE;
    }
    if(s_reader_panel==READER_PANEL_FONT_SETTINGS&&
       (ui_rect_hit(rc_done_rect(),x,y)||ui_rect_hit((EpdRect){32,reader_font_top()+25,64,64},x,y))){
        *handled=true;return s_rc_font_root?rc_to_root(ctx,0):rc_open(ctx,READER_PANEL_NONE);
    }
    if(s_reader_panel==READER_PANEL_LAYOUT_SETTINGS&&
       (ui_rect_hit(reader_layout_back_rect(),x,y)||ui_rect_hit((EpdRect){32,525,64,64},x,y))){*handled=true;return rc_to_root(ctx,0);}
    if(s_reader_panel==READER_PANEL_RULE_SETTINGS&&
       (ui_rect_hit((EpdRect){36,1113,612,72},x,y)||ui_rect_hit((EpdRect){32,605,64,64},x,y))){*handled=true;return rc_to_root(ctx,0);}
    if(s_reader_panel!=READER_PANEL_REFRESH_SETTINGS)return APP_REDRAW_NONE;
    *handled=true;rc_pause_auto();
    if(y<rc_root_top()||ui_rect_hit(rc_done_rect(),x,y))return rc_open(ctx,READER_PANEL_NONE);
    for(int i=0;i<3;++i)if(ui_rect_hit(rc_tab_rect(i),x,y)){s_rc_tab=i;return APP_REDRAW_PAGE;}
    if(s_rc_tab==0){
        for(int i=0;i<4;++i)if(ui_rect_hit(rc_grid_rect(i),x,y)){
            const reader_panel_t panels[]={READER_PANEL_FONT_SETTINGS,READER_PANEL_LAYOUT_SETTINGS,READER_PANEL_READING_DISPLAY,READER_PANEL_RULE_SETTINGS};
            s_rc_font_root=true;return rc_open(ctx,panels[i]);}
    }else if(s_rc_tab==1){
        for(int i=0;i<3;++i)if(ui_rect_hit(rc_effect_rect(i),x,y)){app_settings_set_reader_turn_effect(i);s_turns=0;rc_invalidate();return APP_REDRAW_PAGE;}
        if(ui_rect_hit(rc_refresh_rect(),x,y)){
            static const uint8_t values[]={0,5,10,15,
#if KIIKO_BOARD_METALIO47
                20,
#endif
                30};
            unsigned n=sizeof(values)/sizeof(values[0]),i=0;for(;i<n;++i)if(values[i]==app_settings_reader_full_pages())break;
            app_settings_set_reader_full_pages(values[(i+1)%n]);s_turns=0;return APP_REDRAW_PAGE;
        }
        if(ui_rect_hit(rc_auto_card(0),x,y))return rc_execute(ctx,RC_AUTOTURN);
        for(unsigned plus=0;plus<2;++plus)if(ui_rect_hit(rc_interval_rect(plus),x,y)){
            unsigned seconds=reader_controls_seconds(app_settings_reader_controls());
            if (plus ? seconds >= READER_AUTO_MAX_SECONDS : seconds <= READER_AUTO_MIN_SECONDS)
                return APP_REDRAW_NONE;
            if(!app_settings_set_reader_auto_seconds(plus?seconds+1:seconds-1))rc_notice(ctx,"保存失败，请重试");
            return APP_REDRAW_PAGE;
        }
    }else{
        if(ui_rect_hit((EpdRect){36,746,612,110},x,y)){s_rc_menu_slot=-1;return rc_open(ctx,READER_PANEL_TOUCH_SETTINGS);}
        if(ui_rect_hit((EpdRect){36,878,612,110},x,y))return rc_open(ctx,READER_PANEL_TURN_SETTINGS);
        if(ui_rect_hit((EpdRect){36,1010,612,110},x,y))return rc_open(ctx,READER_PANEL_KEY_SETTINGS);
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t rc_tick(app_ctx_t*ctx){
    /*
     * 逐章缓存的两条等待线：首开等本章落盘、目录里点了未缓存的章。
     * 只读本地状态，300 ms 轮询一次，且只在进度真的变化时重绘——墨水屏上无谓的重绘
     * 比不刷新更伤阅读。两条线都在 body 判定之前，因为等待时页面本身不是可翻的正文。
     * Chapter-cache waits: the first-open chapter and a chapter tapped in the TOC. Poll local
     * state every 300 ms and repaint only on real progress; needless refreshes read worse than
     * none. Both run before the body gate, since a waiting page is not turnable text.
     */
    if (s_warm_chapter >= 0 || s_toc_download >= 0) {
        const bool due = ctx->now_ms >= s_cache_poll_ms;
        /*
         * 首开未缓存的书：后台在第一次下载之前会先问一次云端进度，结果可能把等待的章节
         * 整个换掉（手机上读到第几十章，本地却在等第一章）。这里只做"换目标"，不发起请求；
         * 换完重新计时，卡片上的章号与偏移随之更新。
         * First open with nothing cached: the background asks the cloud once before its first
         * download, and the answer may move the awaited chapter far ahead of chapter one. This
         * only re-targets the wait — it never issues a request — and restarts the timers so the
         * card's chapter number and offset follow.
         */
        wr_seed_t seed = WR_SEED_IDLE;
        if (s_warm_chapter >= 0 && s_warm_enter && due) {
            s_cache_poll_ms = ctx->now_ms + 300;
            unsigned seed_chapter = 0, seed_offset = 0;
            seed = wr_cloud_seed_state(&seed_chapter, &seed_offset);
            if (seed == WR_SEED_FOUND) {
                wr_cloud_seed_ack();
                if (seed_chapter != (unsigned)s_warm_chapter &&
                    seed_chapter < book_chapter_count()) {
                    s_warm_chapter = (int)seed_chapter;
                    s_warm_offset = seed_offset;
                    s_warm_failed = false;
                    s_warm_start_ms = ctx->now_ms;
                    s_cache_poll_ms = 0;
                    s_cache_shown_done = s_cache_shown_percent = 0;
                    (void)book_chapter_cache_request(seed_chapter);
                    return APP_REDRAW_PAGE;
                }
            } else if (seed == WR_SEED_ABSENT) {
                wr_cloud_seed_ack();
                return APP_REDRAW_PAGE;
            } else if (seed == WR_SEED_PENDING && ctx->now_ms - s_warm_start_ms > 10000) {
                // 十秒还没结果（弱网/服务端慢）：不再等云端，按当前章节继续。
                // Ten seconds without an answer (weak link): stop waiting and keep this chapter.
                wr_cloud_seed_ack();
                s_warm_start_ms = ctx->now_ms;
                (void)book_chapter_cache_request((size_t)s_warm_chapter);
                return APP_REDRAW_PAGE;
            }
        }
        if (s_warm_chapter >= 0) {
            const size_t chapter = (size_t)s_warm_chapter;
            if (book_chapter_cache_state(chapter) == BOOK_CH_CACHE_READY) {
                if (wr_virtual_refresh((unsigned)chapter)) {
                    s_warm_chapter = -1;
                    /*
                     * 进正文前把面板与挂起的手势清干净：等待页期间它们既不显示也不该生效，
                     * 带进正文只会让底部工具栏凭空出现，或者拿等待页的坐标翻一页。
                     * Clear panel and pending-tap state before the text appears: neither is
                     * visible or meaningful on the waiting page, and carrying them over pops the
                     * tool rail open or turns a page with coordinates from the card.
                     */
                    s_reader_panel = READER_PANEL_NONE;
                    s_reader_tap.pending = false;
                    /*
                     * 落盘后一律进正文：翻页等来的章也一样，读者不必再翻一次。
                     * 落点按方向定：往后翻进章首；往回翻进本章最后一页（读者想看的是这一章的
                     * 结尾）；云端进度则带着进场时记下的偏移，落到手机上停下的那一句。
                     * Always enter the text once the chapter lands, including one awaited by a
                     * turn; the reader never flips twice. The landing spot follows the direction:
                     * forward to the chapter start, backward to its last page, a cloud position to
                     * the offset captured on entry.
                     */
                    const bool to_end = s_warm_direction < 0 && s_warm_offset == 0;
                    if (load_chapter(ctx, chapter, s_warm_offset, to_end)) return paint_reading(ctx, MODE_GL16);
                    return APP_REDRAW_PAGE;
                }
            } else if (due) {
                s_cache_poll_ms = ctx->now_ms + 300;
                unsigned done = 0, total = 0, percent = 0;
                (void)book_chapter_cache_progress(&done, &total, &percent);
                /*
                 * 任务已经不在跑、这一章却仍没落地，多半是断网或服务端拒绝。
                 * 六秒后把提示改成"未完成"，免得等待页看起来像卡死。
                 * No job is running yet the chapter never landed: usually offline or refused.
                 * After six seconds say so instead of looking hung.
                 */
                if (seed != WR_SEED_PENDING && ctx->now_ms - s_warm_start_ms > 6000 &&
                    wr_status(NULL, 0, NULL, NULL) != WR_BUSY) {
                    /*
                     * 任务已经不在跑、这一章却没落地：断网或被服务端拒绝。以前这里只把卡片
                     * 改成"未完成"就撒手，网络恢复后读者还得自己退出重进——缓存看着就像坏了。
                     * 现在按 8 秒退避重排，四次用尽才让读者退出。
                     * The job is gone yet the chapter never landed: offline or refused. This used
                     * to just relabel the card and stop, so a link that came back still required
                     * leaving and re-entering — the cache looked broken. Re-queue with an 8 s
                     * backoff, four times, before suggesting the reader leave.
                     */
                    const bool retry = s_warm_retries < 4 && ctx->now_ms >= s_warm_retry_ms;
                    if (retry) {
                        ++s_warm_retries;
                        s_warm_retry_ms = ctx->now_ms + 8000;
                        (void)book_chapter_cache_request(chapter);
                    }
                    if (!s_warm_failed || retry) {
                        s_warm_failed = true;
                        return APP_REDRAW_PAGE;
                    }
                }
                if (done != s_cache_shown_done || percent != s_cache_shown_percent) {
                    s_cache_shown_done = done;
                    s_cache_shown_percent = percent;
                    return APP_REDRAW_PAGE;
                }
            }
        }
        if (s_toc_download >= 0 && due) {
            s_cache_poll_ms = ctx->now_ms + 300;
            const book_ch_cache_t state = book_chapter_cache_state(s_toc_download_chapter);
            if (state == BOOK_CH_CACHE_READY) {
                const int target = s_toc_download;
                s_toc_download = -1;
                s_message[0] = 0;
                (void)wr_virtual_refresh((unsigned)s_toc_download_chapter);
                if (toc_open_target(ctx, target)) return APP_REDRAW_PAGE;
            } else if (state == BOOK_CH_CACHE_FAILED ||
                       (ctx->now_ms - s_toc_download_ms > 6000 &&
                        wr_status(NULL, 0, NULL, NULL) != WR_BUSY)) {
                /*
                 * 失败有两种：磁盘校验不符（FAILED），或任务已经结束却没落地（多半断网）。
                 * 都收回到"可重试"，否则目录会一直停在"正在下载"。
                 * Failures come in two shapes: verification mismatch, or the job ended without
                 * landing the chapter (usually offline). Both return to a retryable state.
                 */
                s_toc_download = -1;
                copy_text(s_message, sizeof(s_message), "本章缓存失败，可再点一次重试");
                return APP_REDRAW_PAGE;
            }
        }
        if (s_warm_chapter >= 0 || s_toc_download >= 0) return APP_REDRAW_NONE;
    }
    /*
     * 阅读中持续把"下一章"排上。进入章节时那次提醒可能正撞上 worker 在跑别的任务而被吞掉，
     * 而窗口任务一旦收尾就没有人再提醒——读者翻到下一章才发现它还没缓存，只能当场等。
     * 这里每五秒补一次（缓存层自己去重），保证往前翻时下一章通常已经在盘上。
     * Keep queueing the next chapter while reading. The nudge taken on chapter entry can collide
     * with another job on the worker and be dropped, and once the window job ends nobody asks
     * again — the reader only finds out on the turn, and waits there. Re-queue every five seconds
     * (the cache layer dedupes) so the next chapter is usually already on disk.
     */
    if (s_view == READING && s_text && s_reader_panel == READER_PANEL_NONE && wr_online() &&
        ctx->now_ms >= s_next_prefetch_ms) {
        s_next_prefetch_ms = ctx->now_ms + 5000;
        if (s_chapter + 1 < book_chapter_count() &&
            book_chapter_cache_state(s_chapter + 1) != BOOK_CH_CACHE_READY)
            (void)book_chapter_cache_request(s_chapter + 1);
    }
    /*
     * 目录里"下载中 → 已缓存"的徽标切换：只看落盘计数变没变，变了重画一次。
     * 没有这一步，读者停在目录上等，章节下完了也只能看到"下载中"，必须退出重进。
     * TOC badges going from "下载中" to "已缓存": repaint once whenever the landed-chapter
     * counter moves. Without it a reader sitting on the TOC never sees the change.
     */
    if (s_view == TOC) {
        const unsigned epoch = book_chapter_cache_epoch();
        if (!s_toc_cache_seen) {
            s_toc_cache_seen = true;
            s_toc_cache_epoch = epoch;
            return APP_REDRAW_NONE;
        }
        if (epoch != s_toc_cache_epoch) {
            s_toc_cache_epoch = epoch;
            return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    s_toc_cache_seen = false;
    bool body=s_view==READING&&s_text&&!s_scan_pending&&!s_clear_confirm&&
        (s_reader_panel==READER_PANEL_NONE||s_reader_panel==READER_PANEL_TOOLS)&&s_footnote_note<0&&!rc_zoom_active()&&!s_reader_tap.pending&&!ctx->consumed&&!ctx->request_app&&!ctx->request_menu&&!ctx->request_return;
    if(!body){
        if(!s_text||s_view==SHELF||s_view==BULK||s_view==MANAGE)rc_stop_auto();else rc_pause_auto();
        s_rc_double_pending=false;return APP_REDRAW_NONE;
    }
    if(s_rc_auto.running&&s_rc_auto_waiting){s_rc_auto_waiting=false;reader_auto_start(&s_rc_auto,ctx->now_ms,reader_controls_seconds(app_settings_reader_controls()));}
    if(s_rc_double_pending&&ctx->now_ms>=s_rc_double_until&&!(ctx->touch&&ctx->touch->touched)){
        s_rc_double_pending=false;return rc_execute(ctx,app_settings_reader_controls()->actions[RC_TAP_CENTER]);
    }
    if(ctx->touch&&ctx->touch->touched)return APP_REDRAW_NONE;
    bool end=s_page+1>=book_layout_page_count()&&s_chapter+1>=book_chapter_count();
    if(end&&s_rc_auto.running){rc_stop_auto();rc_notice(ctx,"已到书末，自动翻页已停止");return APP_REDRAW_PAGE;}
    if(reader_auto_due(&s_rc_auto,ctx->now_ms,reader_controls_seconds(app_settings_reader_controls()),body,end)){
        size_t chapter=s_chapter,page=s_page;s_rc_auto_turning=true;app_redraw_t redraw=turn_page(ctx,1);s_rc_auto_turning=false;
        if(s_view!=READING||!s_text||(chapter==s_chapter&&page==s_page))rc_stop_auto();
        else ctx->user_activity=true;
        return redraw;
    }
    /*
     * 分章缓存续读：刚打开的书可能只有占位正文，后台把这一章取回后由这里原地重排，
     * 读者不需要退出重进。先记住当前页在章节内的起始偏移，重排后落回同一段文字。
     * Chapter-cache handoff: a freshly opened book may show placeholder text; once the
     * background fetch lands this chapter, re-layout in place at the same source offset so the
     * reader never has to leave and re-enter the book.
     */
    int landed=wr_read_take_ready();
    if(landed>=0&&(size_t)landed==s_chapter&&s_text){
        size_t keep=book_layout_page_start_offset(s_page);
        if(wr_virtual_refresh((unsigned)landed)&&load_chapter(ctx,s_chapter,keep,false))
            return paint_reading(ctx,MODE_DU);
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t rc_bottom_swipe(app_ctx_t*ctx,const ui_gesture_event_t*ev,bool*handled){
    *handled=false;
    if(s_view!=READING||!s_text||s_clear_confirm||s_scan_pending||ctx->consumed||
       (s_reader_panel!=READER_PANEL_NONE&&s_reader_panel!=READER_PANEL_TOOLS)||
       ev->type!=UI_GESTURE_SWIPE_U||ev->y0<1096||ev->y0>=1216||ev->x0>=684)return APP_REDRAW_NONE;
    *handled=true;unsigned slot=RC_BOTTOM_LEFT+ev->x0/228;
    s_pressed_control=s_reader_slider=-1;s_reader_tap.pending=false;
    app_redraw_t redraw=rc_execute(ctx,app_settings_reader_controls()->actions[slot]);
    return redraw==APP_REDRAW_NONE?APP_REDRAW_PAGE:redraw;
}
static bool rc_custom_modal(void){return s_view==READING&&(s_reader_panel==READER_PANEL_REFRESH_SETTINGS||s_reader_panel==READER_PANEL_READING_DISPLAY||s_reader_panel==READER_PANEL_TOUCH_SETTINGS||s_reader_panel==READER_PANEL_TOUCH_MORE||s_reader_panel==READER_PANEL_READING_LIGHT);}
static int rc_hit_target(int x,int y){
    if(s_rc_menu_slot>=0){EpdRect r=rc_menu_rect();if(!ui_rect_hit(r,x,y))return 999;
        bool side=s_rc_menu_slot>=RC_SLOT_COUNT;if(!side&&y<r.y+52)return 400+(x>=r.x+r.width/2);
        return 500+(y-r.y-(side?6:54))/48;}
    if(s_reader_panel==READER_PANEL_TOUCH_SETTINGS){
        if(ui_rect_hit((EpdRect){608,14,64,64},x,y))return 999;
        if(ui_rect_hit((EpdRect){12,14,64,64},x,y))return 998;
        for(unsigned i=0;i<RC_SLOT_COUNT+2;++i)if(ui_rect_hit(rc_touch_rect(i),x,y))return 100+i;
        return -1;
    }
    if(s_reader_panel==READER_PANEL_TOUCH_MORE){
        if(ui_rect_hit((EpdRect){608,14,64,64},x,y)||ui_rect_hit(rc_done_rect(),x,y))return 999;
        for(unsigned i=0;i<11;++i)if(rc_more_slots[i]>=0&&ui_rect_hit(rc_more_rect(i),x,y))return 100+i;
        return -1;
    }
    if(s_reader_panel==READER_PANEL_READING_DISPLAY){
        if(y<826||ui_rect_hit(rc_done_rect(),x,y))return 999;
        for(int i=0;i<4;++i)if(ui_rect_hit(rc_display_rect(i),x,y))return 100+i;
        return -1;
    }
    if(s_reader_panel==READER_PANEL_READING_LIGHT){if(y<876||ui_rect_hit(rc_done_rect(),x,y))return 999;return -1;}
    if(s_reader_panel==READER_PANEL_REFRESH_SETTINGS){
        if(y<rc_root_top()||ui_rect_hit(rc_done_rect(),x,y))return 999;
        for(int i=0;i<3;++i)if(ui_rect_hit(rc_tab_rect(i),x,y))return 10+i;
        if(s_rc_tab==0){for(int i=0;i<4;++i)if(ui_rect_hit(rc_grid_rect(i),x,y))return 100+i;}
        if(s_rc_tab==1){for(int i=0;i<3;++i)if(ui_rect_hit(rc_effect_rect(i),x,y))return 100+i;
            if(ui_rect_hit(rc_refresh_rect(),x,y))return 110;
            if(ui_rect_hit(rc_auto_card(0),x,y))return 111;
            for(unsigned i=0;i<2;++i)if(ui_rect_hit(rc_interval_rect(i),x,y))return 112+i;}
        if(s_rc_tab==2){for(int i=0;i<3;++i)if(ui_rect_hit((EpdRect){36,746+i*132,612,110},x,y))return 100+i;}
        return -1;
    }
    return -1;
}
static void rc_cancel_side(void){
#if KIIKO_BOARD_METALIO47
    if(s_rc_side_active&&s_rc_side_captured)metalio_frontlight_restore();
#endif
    s_rc_side_active=s_rc_side_captured=false;
}
static app_redraw_t rc_gesture(app_ctx_t*ctx,const ui_gesture_event_t*ev,bool*handled){
    *handled=false;
    if(rc_custom_modal()){
        *handled=true;rc_pause_auto();
#if KIIKO_BOARD_METALIO47
        if(s_reader_panel==READER_PANEL_READING_LIGHT){
            EpdRect slider=rc_light_slider_rect();
            if(ev->type==UI_GESTURE_PRESS)s_rc_light_drag=ui_rect_hit(slider,ev->x0,ev->y0);
            if(s_rc_light_drag){
                if(ev->type==UI_GESTURE_CANCEL){s_rc_light_drag=false;metalio_frontlight_restore();return APP_REDRAW_PAGE;}
                int level=((int)ev->x-slider.x-64)*METALIO_FRONTLIGHT_RANGE/(slider.width-128);if(level<0)level=0;if(level>METALIO_FRONTLIGHT_RANGE)level=METALIO_FRONTLIGHT_RANGE;
                metalio_frontlight_preview_position(s_rc_light_warm?metalio_frontlight_position(false):(unsigned)level,s_rc_light_warm?(unsigned)level:metalio_frontlight_position(true));
                if(ev->type!=UI_GESTURE_MOVE&&ev->type!=UI_GESTURE_PRESS){s_rc_light_drag=false;metalio_frontlight_commit();}
                render(ctx,ctx->fb);s_area=(EpdRect){36,930,612,120};s_mode=MODE_GL16;return APP_REDRAW_AREA;
            }
        }
#endif
        if(ev->type==UI_GESTURE_PRESS){s_rc_press_target=rc_hit_target(ev->x0,ev->y0);return APP_REDRAW_NONE;}
        if(ev->type==UI_GESTURE_TAP){int end=rc_hit_target(ev->x,ev->y);if(end>=0&&end==s_rc_press_target){bool used;return rc_panel_action(ctx,ev->x,ev->y,&used);}}
        return APP_REDRAW_NONE;
    }
    if(s_view!=READING||!s_text||s_clear_confirm||s_scan_pending||ctx->consumed||
       (s_reader_panel!=READER_PANEL_NONE&&s_reader_panel!=READER_PANEL_TOOLS)){
        rc_cancel_side();return APP_REDRAW_NONE;
    }
    if(ev->type==UI_GESTURE_PRESS){
        s_rc_side_active=false;s_rc_side_captured=false;
        if(ev->y0>=176&&ev->y0<1096&&(ev->x0<56||ev->x0>=628)){
            s_rc_side_index=ev->x0>=628;s_rc_side_action=app_settings_reader_controls()->sides[s_rc_side_index];
            s_rc_side_active=s_rc_side_action!=RC_SIDE_OFF;s_rc_side_value=s_rc_side_action==RC_SIDE_FONT?s_px:app_settings_book_line_spacing();
#if KIIKO_BOARD_METALIO47
            if(s_rc_side_action==RC_SIDE_BRIGHTNESS||s_rc_side_action==RC_SIDE_WARMTH)s_rc_side_value=metalio_frontlight_position(s_rc_side_action==RC_SIDE_WARMTH);
#endif
            s_rc_side_start=s_rc_side_value;
        }
    }
    int dx=(int)ev->x-ev->x0,dy=(int)ev->y-ev->y0;
    if(s_rc_side_active&&(s_rc_side_captured||(ev->type==UI_GESTURE_MOVE&&abs(dy)>24&&abs(dy)>abs(dx)))){
        *handled=true;s_rc_side_captured=true;rc_pause_auto();
        if(ev->type==UI_GESTURE_CANCEL){rc_cancel_side();return APP_REDRAW_PAGE;}
        int min=0,max=240,step=2;
        if(s_rc_side_action==RC_SIDE_FONT){min=BOOK_PX_MIN;max=BOOK_PX_MAX;step=24;}
        if(s_rc_side_action==RC_SIDE_LINE){min=110;max=150;step=24;}
        int value=s_rc_side_start-dy/step;if(value<min)value=min;if(value>max)value=max;
        bool changed=value!=s_rc_side_value;s_rc_side_value=value;
#if KIIKO_BOARD_METALIO47
        if((s_rc_side_action==RC_SIDE_BRIGHTNESS||s_rc_side_action==RC_SIDE_WARMTH)&&changed)
            metalio_frontlight_preview_position(s_rc_side_action==RC_SIDE_BRIGHTNESS?value:metalio_frontlight_position(false),s_rc_side_action==RC_SIDE_WARMTH?value:metalio_frontlight_position(true));
#endif
        if(ev->type!=UI_GESTURE_MOVE&&ev->type!=UI_GESTURE_PRESS){
            s_rc_side_active=s_rc_side_captured=false;
#if KIIKO_BOARD_METALIO47
            if(s_rc_side_action==RC_SIDE_BRIGHTNESS||s_rc_side_action==RC_SIDE_WARMTH)metalio_frontlight_commit();
#endif
            if(s_rc_side_action==RC_SIDE_FONT)return apply_reader_layout(ctx,value,s_margin,app_settings_book_line_spacing(),app_settings_book_paragraph_spacing());
            if(s_rc_side_action==RC_SIDE_LINE)return apply_reader_layout(ctx,s_px,s_margin,value,app_settings_book_paragraph_spacing());
            return APP_REDRAW_PAGE;
        }
        if(changed){char text[64];snprintf(text,sizeof(text),"%s %d",rc_side_name(s_rc_side_action),value);rc_notice(ctx,text);render(ctx,ctx->fb);s_area=(EpdRect){208,150,268,68};s_mode=MODE_GL16;return APP_REDRAW_AREA;}
        return APP_REDRAW_NONE;
    }
    if(ev->type==UI_GESTURE_CANCEL)rc_cancel_side();
    if(ev->y0<1096&&ui_rect_hit(body_rect(),ev->x0,ev->y0)&&!reader_zoom_mode()){
        if(ev->type==UI_GESTURE_SWIPE_L||ev->type==UI_GESTURE_SWIPE_R){*handled=true;rc_cancel_side();return rc_execute(ctx,ev->type==UI_GESTURE_SWIPE_L?RC_NEXT:RC_PREVIOUS);}
        if(ev->type==UI_GESTURE_SWIPE_U||ev->type==UI_GESTURE_SWIPE_D){*handled=true;rc_cancel_side();return rc_execute(ctx,ev->type==UI_GESTURE_SWIPE_U?RC_NEXT:RC_PREVIOUS);}
        if(ev->type==UI_GESTURE_LONG_PRESS&&!reader_page_image_hit(ev->x0,ev->y0)&&s_footnote_note<0){*handled=true;return rc_execute(ctx,app_settings_reader_controls()->actions[RC_HOLD]);}
        if(ev->type==UI_GESTURE_MOVE&&abs(dy)>24)rc_pause_auto();
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t rc_body_tap(app_ctx_t*ctx,int x,int y,bool image,bool*handled){
    *handled=false;
    if(image||s_clear_confirm||(s_reader_panel!=READER_PANEL_NONE&&s_reader_panel!=READER_PANEL_TOOLS)||!ui_rect_hit(body_rect(),x,y))return APP_REDRAW_NONE;
    const reader_controls_config_t*c=app_settings_reader_controls();bool center=book_reader_center(body_rect(),x,y);
    if(center&&c->actions[RC_DOUBLE_CENTER]!=RC_NONE){
        *handled=true;rc_pause_auto();
        if(s_rc_double_pending&&ctx->now_ms<=s_rc_double_until){s_rc_double_pending=false;return rc_execute(ctx,c->actions[RC_DOUBLE_CENTER]);}
        s_rc_double_pending=true;s_rc_double_until=ctx->now_ms+300;return APP_REDRAW_NONE;
    }
    if(center&&(!s_reader_fullscreen||c->actions[RC_TAP_CENTER]!=RC_FULLSCREEN)){
        *handled=true;return rc_execute(ctx,c->actions[RC_TAP_CENTER]);
    }
    EpdRect r=body_rect();bool previous=app_settings_reader_vertical_turn()?y<r.y+r.height/3:x<r.x+r.width/2;
    unsigned slot=previous?RC_TAP_PREVIOUS:RC_TAP_NEXT,action=c->actions[slot];
    if(action!=(unsigned)(previous?RC_PREVIOUS:RC_NEXT)){*handled=true;return rc_execute(ctx,action);}
    return APP_REDRAW_NONE;
}
