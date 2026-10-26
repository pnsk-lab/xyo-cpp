// Included inside Editor::runBlockEditSelfTest(). Exercise complete mouse
// gestures, not doDrop() alone: state leaking between gestures was the bug.
{
    Blocks sandbox(rt);
    Blocks* savedBlocks = t->blocks;
    const int savedCat = catSel;
    const float savedPanX = panX, savedPanY = panY, savedZoom = zoom;
    const int savedTab = tab;
    t->blocks = &sandbox;
    tab = 0;
    catSel = 0;
    zoom = 1.f;
    panX = ui.ws.x + 30.f;
    panY = ui.ws.y + 30.f;
    drag = Drag{};
    selBlock.clear();
    menu_.open = ctx_.open = topMenu_.open = mb_.open = false;
    dirPicker_.open = cp_.open = am_.open = false;
    edit_.active = spEdit_.active = nmEdit_.active = false;
    panning = false;
    scrollDrag_ = 0;
    clearHistory();
    flyScroll = 0;
    rebuildFlyout();

    auto make = [&](const char* opcode, float x, float y) {
        return instBlock(&sandbox, opcode, {}, x, y, true);
    };
    const std::string anchor = make("motion_movesteps", 80, 100);
    const std::string oldNext = make("motion_turnright", 80, 148);
    sandbox.deleteScript(oldNext);
    sandbox.getBlock(oldNext)->topLevel = false;
    sandbox.getBlock(oldNext)->parent = anchor;
    sandbox.getBlock(anchor)->next = oldNext;
    const std::string movedHead = make("motion_movesteps", 430, 130);
    const std::string movedTail = make("motion_turnright", 430, 178);
    sandbox.deleteScript(movedTail);
    sandbox.getBlock(movedTail)->topLevel = false;
    sandbox.getBlock(movedTail)->parent = movedHead;
    sandbox.getBlock(movedHead)->next = movedTail;
    sandbox.resetCache();
    wsDirty_ = true;
    rebuildWs();

    auto mouseBtn = [&](Uint32 type, float x, float y, Uint8 button) {
        SDL_Event ev{};
        ev.type = type;
        if (type == SDL_MOUSEMOTION) {
            ev.motion.x = (int)std::lround(x);
            ev.motion.y = (int)std::lround(y);
            ev.motion.state = (button == SDL_BUTTON_MIDDLE)
                                  ? SDL_BUTTON_MMASK
                                  : SDL_BUTTON_LMASK;
        } else {
            ev.button.button = button;
            ev.button.x = (int)std::lround(x);
            ev.button.y = (int)std::lround(y);
        }
        // Exercise SDL's queue/conversion and the same polling path as real input.
        ev.motion.windowID = SDL_GetWindowID(win);
        check(SDL_PushEvent(&ev) == 1, "gesture: mouse event queued");
        SDL_Event queued{};
        bool delivered = false;
        while (SDL_PollEvent(&queued)) {
            if (queued.type == type && queued.motion.windowID == ev.motion.windowID) {
                delivered = true;
                check(handleEvent(queued), "gesture: polled mouse event accepted");
            }
        }
        check(delivered, "gesture: mouse event survives SDL polling");
    };
    auto mouse = [&](Uint32 type, float x, float y) {
        mouseBtn(type, x, y, SDL_BUTTON_LEFT);
    };
    auto sx = [&](float x) { return panX + x * wsScale(); };
    auto sy = [&](float y) { return panY + y * wsScale(); };

    // Move an existing two-block stack first: this populates Drag::tailId.
    mouse(SDL_MOUSEBUTTONDOWN, sx(442), sy(140));
    mouse(SDL_MOUSEMOTION, sx(412), sy(460));
    check(drag.active && drag.headId == movedHead,
          "gesture: existing chain drag started");
    mouse(SDL_MOUSEBUTTONUP, sx(412), sy(460));
    check(sandbox.getBlock(movedHead)->next == movedTail &&
          sandbox.getBlock(movedTail)->next.empty(),
          "gesture: standalone moved chain is intact");
    wsDirty_ = true;
    rebuildWs();

    // Then use the palette twice. Neither new block may reuse movedTail.
    auto paletteInsert = [&](const std::string& expectedNext) {
        rebuildFlyout();
        FlyItem* item = nullptr;
        for (auto& f : flyItems_)
            if (f.kind == FlyItem::BlockT && f.opcode == "motion_movesteps") {
                item = &f;
                break;
            }
        check(item != nullptr, "gesture: motion palette entry exists");
        if (!item) return std::string{};
        float fx = ui.flyout.x + 20.f;
        float fy = ui.flyout.y + item->y * bl::kWsZoom - flyScroll + 10.f;
        mouse(SDL_MOUSEBUTTONDOWN, fx, fy);
        check(drag.pending && drag.fromFlyout,
              "gesture: palette drag starts after prior gesture");
        float ax = 0.f, ay = 0.f;
        bool found = false;
        for (auto& lb : wsLayout_.blocks) if (lb.id == anchor) {
            ax = lb.x;
            ay = lb.y + lb.h;
            found = true;
            break;
        }
        check(found, "gesture: anchor has current layout");
        if (!found) return std::string{};
        const float dropX = sx(ax + drag.offX);
        const float dropY = sy(ay + drag.offY);
        mouse(SDL_MOUSEMOTION, dropX, dropY);
        check(drag.active && drag.targetId == anchor && !drag.before,
              "gesture: palette block snaps after anchor");
        mouse(SDL_MOUSEBUTTONUP, dropX, dropY);
        std::string inserted = sandbox.getBlock(anchor)->next;
        Block* nb = sandbox.getBlock(inserted);
        check(nb && inserted != expectedNext && inserted != movedTail &&
              nb->opcode == "motion_movesteps" && nb->parent == anchor &&
              nb->next == expectedNext && !nb->topLevel,
              "gesture: palette insertion keeps displaced successor");
        Block* successor = sandbox.getBlock(expectedNext);
        check(successor && successor->parent == inserted,
              "gesture: displaced successor parent updated");
        check(sandbox.getBlock(movedHead)->next == movedTail &&
              sandbox.getBlock(movedTail)->next.empty(),
              "gesture: palette insertion cannot mutate previous drag tail");
        std::set<std::string> visited;
        std::string id = anchor;
        bool cycle = false;
        for (int n = 0; !id.empty() && n < 32; ++n) {
            if (!visited.insert(id).second) { cycle = true; break; }
            Block* b = sandbox.getBlock(id);
            if (!b) break;
            id = b->next;
        }
        check(!cycle && id.empty(), "gesture: inserted chain is finite and acyclic");
        wsDirty_ = true;
        rebuildWs();
        return inserted;
    };
    const std::string firstInserted = paletteInsert(oldNext);
    if (!firstInserted.empty()) paletteInsert(firstInserted);

    // The first block of a nonempty C-mouth has no .next predecessor.
    // Inserting before it must rewrite the owning SUBSTACK input, not add a
    // new top-level script while leaving the owner's old child in place.
    auto prependBranch = [&](const char* opcode, int branch) {
        Blocks branchSandbox(rt);
        t->blocks = &branchSandbox;
        drag = Drag{};
        clearHistory();
        std::string owner = instBlock(&branchSandbox, opcode, {}, 100, 220, true);
        std::string child = instBlock(&branchSandbox, "motion_turnright", {},
                                     0, 0, false);
        const char* inputName = branch == 0 ? "SUBSTACK" : "SUBSTACK2";
        BlockInput* input = branchSandbox.getBlock(owner)->inputs.find(inputName);
        check(input != nullptr, "gesture: C-mouth input exists");
        if (input) input->block = child;
        branchSandbox.getBlock(child)->parent = owner;
        branchSandbox.resetCache();
        wsDirty_ = true;
        rebuildWs();
        rebuildFlyout();
        const size_t scriptCount = branchSandbox.getScripts().size();
        float childX = 0.f, childY = 0.f;
        bool childLaidOut = false;
        for (auto& lb : wsLayout_.blocks) if (lb.id == child) {
            childX = lb.x; childY = lb.y; childLaidOut = true; break;
        }
        check(childLaidOut, "gesture: first C-mouth child laid out");
        FlyItem* paletteItem = nullptr;
        for (auto& f : flyItems_)
            if (f.kind == FlyItem::BlockT && f.opcode == "motion_movesteps") {
                paletteItem = &f; break;
            }
        if (childLaidOut && paletteItem) {
            float fy = ui.flyout.y + paletteItem->y * bl::kWsZoom - flyScroll + 10.f;
            mouse(SDL_MOUSEBUTTONDOWN, ui.flyout.x + 20.f, fy);
            float dx = sx(childX + drag.offX);
            float dy = sy(childY + drag.offY);
            mouse(SDL_MOUSEMOTION, dx, dy);
            check(drag.active &&
                  ((drag.targetId == child && drag.before) ||
                   (drag.targetId == owner && drag.substackTarget &&
                    drag.subBranch == branch)),
                  "gesture: drop targets beginning of occupied C-mouth");
            mouse(SDL_MOUSEBUTTONUP, dx, dy);
            input = branchSandbox.getBlock(owner)->inputs.find(inputName);
            std::string inserted = input ? input->block : std::string{};
            Block* nb = branchSandbox.getBlock(inserted);
            check(nb && inserted != child && nb->parent == owner &&
                  nb->next == child && !nb->topLevel,
                  "gesture: prepending C-mouth rewires owner input");
            check(branchSandbox.getBlock(child)->parent == inserted,
                  "gesture: original C-mouth head parent updated");
            check(branchSandbox.getScripts().size() == scriptCount,
                  "gesture: C-mouth prepend does not add a top-level script");
        } else {
            check(false, "gesture: C-mouth palette entry exists");
        }
        drag = Drag{};
        clearHistory();
        t->blocks = &sandbox;
        wsDirty_ = true;
        rebuildWs();
    };
    prependBranch("control_repeat", 0);
    prependBranch("control_if_else", 0);
    prependBranch("control_if_else", 1);

    // Value reporters are emitted after their containing block, unlike C-mouth
    // statement children. Mouse picking must select the deepest real block.
    {
        Blocks valueSandbox(rt);
        t->blocks = &valueSandbox;
        drag = Drag{};
        const std::string owner = instBlock(&valueSandbox, "motion_movesteps", {},
                                           100, 220, true);
        const std::string reporter = instBlock(&valueSandbox, "operator_add", {},
                                              0, 0, false);
        bl::attachToValue(&valueSandbox, reporter, owner, "STEPS");
        valueSandbox.resetCache();
        wsDirty_ = true;
        rebuildWs();
        bool foundReporter = false;
        for (auto& lb : wsLayout_.blocks) if (lb.id == reporter) {
            const float x = sx(lb.x + lb.w / 2);
            const float y = sy(lb.y + lb.h / 2);
            bl::LBlock* picked = wsBlockAt(x, y);
            check(picked && picked->id == reporter,
                  "gesture: embedded real reporter wins parent hit test");
            mouse(SDL_MOUSEBUTTONDOWN, x, y);
            check(drag.pending && drag.headId == reporter,
                  "gesture: clicking embedded reporter starts its own gesture");
            foundReporter = true;
            break;
        }
        check(foundReporter, "gesture: embedded reporter laid out");
        drag = Drag{};
        t->blocks = &sandbox;
        wsDirty_ = true;
        rebuildWs();
    }

    // Hats track the grabbed point through pan/zoom, connect by the last
    // block's bottom, and keep the new root pose after insertBefore/re-layout.
    for (float testZoom : {0.65f, 1.3f}) {
        Blocks hatSandbox(rt);
        t->blocks = &hatSandbox;
        drag = Drag{};
        zoom = testZoom;
        panX = ui.ws.x + 10.f;
        panY = ui.ws.y + 10.f;
        const std::string hat = instBlock(&hatSandbox, "event_whenflagclicked", {},
                                         40, 40, true);
        const std::string body = instBlock(&hatSandbox, "motion_movesteps", {},
                                          0, 0, false);
        hatSandbox.getBlock(hat)->next = body;
        hatSandbox.getBlock(body)->parent = hat;
        const std::string target = instBlock(&hatSandbox, "motion_turnright", {},
                                            220, 420, true);
        hatSandbox.resetCache();
        wsDirty_ = true;
        rebuildWs();
        // chainHeight includes the final bump tip; connector coordinates do
        // not. Use the same origin-relative chain layout as the drag cache.
        std::vector<bl::LBlock> expectedLayout;
        const float height = bl::layoutChain(&hatSandbox, hat, 0, 0, tr(),
                                             expectedLayout, 0);
        const float desiredX = 220, desiredY = 420 - height;
        mouse(SDL_MOUSEBUTTONDOWN, sx(60), sy(68));
        const float grabX = drag.offX, grabY = drag.offY;
        mouse(SDL_MOUSEMOTION, sx(desiredX + grabX), sy(desiredY + grabY));
        check(drag.active && drag.headId == hat,
              "hat: existing hat stack follows drag");
        check(std::fabs((drag.mx + grabX) * wsScale() + panX -
                        std::lround(sx(desiredX + grabX))) < 0.1f &&
              std::fabs((drag.my + grabY) * wsScale() + panY -
                        std::lround(sy(desiredY + grabY))) < 0.1f,
              "hat: grabbed point follows pointer under pan and zoom");
        check(drag.targetId == target && drag.before && drag.hasSnap,
              "hat: last block bottom snaps before a standalone script");
        mouse(SDL_MOUSEBUTTONUP, sx(desiredX + grabX), sy(desiredY + grabY));
        Block* hb = hatSandbox.getBlock(hat);
        check(hb && hb->topLevel && hb->parent.empty() &&
              std::fabs(hb->x - desiredX) < 0.01f &&
              std::fabs(hb->y - desiredY) < 0.01f,
              "hat: newly promoted root keeps snapped position after release");
        check(hatSandbox.getBlock(body)->next == target &&
              hatSandbox.getBlock(target)->parent == body,
              "hat: outgoing tail connection preserves existing hat body");
        rebuildWs();
        bool laidOut = false;
        for (auto& lb : wsLayout_.blocks) if (lb.id == hat) {
            laidOut = true;
            check(std::fabs(lb.x - desiredX) < 0.01f &&
                  std::fabs(lb.y - desiredY) < 0.01f,
                  "hat: re-layout does not jump back to pre-drag coordinates");
        }
        check(laidOut, "hat: connected root remains laid out");
        bool tailAligned = false;
        for (const auto& lb : wsLayout_.blocks) if (lb.id == body) {
            tailAligned = true;
            check(std::fabs(lb.x - 220) < 0.01f &&
                  std::fabs(lb.y + lb.h - 420) < 0.01f,
                  "hat: outgoing tail connector remains at target's original position");
        }
        check(tailAligned, "hat: connected tail remains laid out");

        // A final release can arrive without a corresponding motion event.
        mouse(SDL_MOUSEBUTTONDOWN, sx(desiredX + 20), sy(desiredY + 28));
        const float finalGrabX = drag.offX, finalGrabY = drag.offY;
        mouse(SDL_MOUSEMOTION, sx(120 + finalGrabX), sy(130 + finalGrabY));
        mouse(SDL_MOUSEMOTION, sx(135 + finalGrabX), sy(150 + finalGrabY));
        check(std::fabs(drag.mx - 135) * wsScale() <= 0.51f &&
              std::fabs(drag.my - 150) * wsScale() <= 0.51f,
              "hat: successive pointer motions update root position");
        mouse(SDL_MOUSEBUTTONUP, sx(145 + finalGrabX), sy(170 + finalGrabY));
        hb = hatSandbox.getBlock(hat);
        check(std::fabs(hb->x - 145) * wsScale() <= 0.51f &&
              std::fabs(hb->y - 170) * wsScale() <= 0.51f,
              "hat: last release coordinates persist without a motion event");
        drag = Drag{};
        t->blocks = &sandbox;
        wsDirty_ = true;
        rebuildWs();
    }
    {
        Blocks hatSandbox(rt);
        t->blocks = &hatSandbox;
        drag = Drag{};
        zoom = 1.f;
        panX = ui.ws.x + 10.f;
        panY = ui.ws.y + 10.f;
        const std::string hat = instBlock(&hatSandbox, "event_whenflagclicked", {},
                                         40, 40, true);
        const std::string body = instBlock(&hatSandbox, "motion_movesteps", {},
                                          0, 0, false);
        hatSandbox.getBlock(hat)->next = body;
        hatSandbox.getBlock(body)->parent = hat;
        const std::string target = instBlock(&hatSandbox, "motion_movesteps", {},
                                            260, 260, true);
        hatSandbox.resetCache();
        wsDirty_ = true;
        rebuildWs();
        const float targetH = bl::measureBlock(&hatSandbox,
                                               hatSandbox.getBlock(target), tr()).h;
        mouse(SDL_MOUSEBUTTONDOWN, sx(60), sy(68));
        const float grabX = drag.offX, grabY = drag.offY;
        mouse(SDL_MOUSEMOTION, sx(260 + grabX), sy(260 + targetH + grabY));
        check(drag.targetId.empty(), "hat: no incoming connector below another block");
        mouse(SDL_MOUSEBUTTONUP, sx(260 + grabX), sy(260 + targetH + grabY));
        check(hatSandbox.getBlock(hat)->topLevel &&
              hatSandbox.getBlock(hat)->parent.empty() &&
              hatSandbox.getBlock(target)->next.empty(),
              "hat: body-drop fallback cannot reparent a hat");
        drag = Drag{};
        t->blocks = &sandbox;
        wsDirty_ = true;
        rebuildWs();
    }

    // The visible dome is above the hat's logical y origin. Grabbing it
    // produces a negative offY and must track the same visible point.
    for (float testZoom : {0.65f, 1.3f}) {
        Blocks domeSandbox(rt);
        t->blocks = &domeSandbox;
        drag = Drag{};
        zoom = testZoom;
        panX = ui.ws.x + 10.f;
        panY = ui.ws.y + 10.f;
        const std::string hat = instBlock(&domeSandbox, "event_whenflagclicked", {},
                                         120, 120, true);
        domeSandbox.resetCache();
        wsDirty_ = true;
        rebuildWs();
        const float domeX = sx(120 + 48), domeY = sy(120 - 10);
        bl::LBlock* picked = wsBlockAt(domeX, domeY);
        check(picked && picked->id == hat, "hat dome: above-origin dome is hittable");
        mouse(SDL_MOUSEBUTTONDOWN, domeX, domeY);
        check(drag.pending && drag.headId == hat && drag.offY < 0,
              "hat dome: queued press grabs root with negative vertical offset");
        const float grabX = drag.offX, grabY = drag.offY;
        mouse(SDL_MOUSEMOTION, sx(160 + grabX), sy(180 + grabY));
        mouse(SDL_MOUSEMOTION, sx(190 + grabX), sy(240 + grabY));
        check(drag.active && drag.headId == hat && drag.offY == grabY,
              "hat dome: negative grabbed offset survives successive motions");
        check(std::fabs((drag.mx + grabX) * wsScale() + panX -
                        std::lround(sx(190 + grabX))) < 0.1f &&
              std::fabs((drag.my + grabY) * wsScale() + panY -
                        std::lround(sy(240 + grabY))) < 0.1f,
              "hat dome: clicked dome point continues following the pointer");
        mouse(SDL_MOUSEBUTTONUP, sx(210 + grabX), sy(260 + grabY));
        Block* hb = domeSandbox.getBlock(hat);
        check(hb && hb->topLevel && hb->parent.empty() &&
              std::fabs(hb->x - 210) * wsScale() <= 0.51f &&
              std::fabs(hb->y - 260) * wsScale() <= 0.51f,
              "hat dome: standalone release persists dragged root position");
        drag = Drag{};
        t->blocks = &sandbox;
        wsDirty_ = true;
        rebuildWs();
    }

    // Camera movement must work with an ordinary mouse: plain left-drag on
    // empty canvas, middle-drag, and space + left-drag over blocks.
    {
        Blocks emptySandbox(rt);
        t->blocks = &emptySandbox;
        drag = Drag{};
        zoom = 1.f;
        panX = ui.ws.x + 30.f;
        panY = ui.ws.y + 30.f;
        wsDirty_ = true;
        rebuildWs();
        const float cx = ui.ws.x + ui.ws.w / 2.f;
        const float cy = ui.ws.y + ui.ws.h / 2.f;

        const float px0 = panX, py0 = panY;
        mouse(SDL_MOUSEBUTTONDOWN, cx, cy);
        check(panning && panButton == SDL_BUTTON_LEFT,
              "pan: left drag on empty canvas starts camera pan");
        mouse(SDL_MOUSEMOTION, cx + 60.f, cy + 40.f);
        check(std::fabs(panX - (px0 + 60.f)) < 0.01f &&
              std::fabs(panY - (py0 + 40.f)) < 0.01f,
              "pan: camera follows the cursor");
        mouse(SDL_MOUSEBUTTONUP, cx + 60.f, cy + 40.f);
        check(!panning, "pan: release ends camera pan");

        const float px1 = panX, py1 = panY;
        mouse(SDL_MOUSEBUTTONDOWN, cx, cy);
        mouse(SDL_MOUSEBUTTONUP, cx, cy);
        check(!panning && std::fabs(panX - px1) < 0.01f &&
              std::fabs(panY - py1) < 0.01f,
              "pan: a plain click does not move the camera");

        mouseBtn(SDL_MOUSEBUTTONDOWN, cx, cy, SDL_BUTTON_MIDDLE);
        mouseBtn(SDL_MOUSEMOTION, cx - 35.f, cy - 20.f, SDL_BUTTON_MIDDLE);
        mouseBtn(SDL_MOUSEBUTTONUP, cx - 35.f, cy - 20.f, SDL_BUTTON_MIDDLE);
        check(!panning && std::fabs(panX - (px1 - 35.f)) < 0.01f &&
              std::fabs(panY - (py1 - 20.f)) < 0.01f,
              "pan: middle-drag still moves the camera");

        // Space + left drag pans even when the pointer starts on a block.
        const std::string blocker = instBlock(&emptySandbox, "motion_movesteps",
                                              {}, 40, 40, true);
        emptySandbox.resetCache();
        wsDirty_ = true;
        rebuildWs();
        const float bx = ui.ws.x + 30.f + 60.f;
        const float by = ui.ws.y + 30.f + 60.f;
        SDL_Event kev{};
        kev.type = SDL_KEYDOWN;
        kev.key.keysym.sym = SDLK_SPACE;
        check(handleEvent(kev), "pan: space key accepted");
        const float px2 = panX, py2 = panY;
        mouse(SDL_MOUSEBUTTONDOWN, bx, by);
        check(panning && panButton == SDL_BUTTON_LEFT,
              "pan: space + left drag pans over blocks");
        mouse(SDL_MOUSEMOTION, bx + 25.f, by + 15.f);
        mouse(SDL_MOUSEBUTTONUP, bx + 25.f, by + 15.f);
        kev.type = SDL_KEYUP;
        check(handleEvent(kev), "pan: space release accepted");
        check(!drag.active && !drag.pending &&
              std::fabs(panX - (px2 + 25.f)) < 0.01f &&
              std::fabs(panY - (py2 + 15.f)) < 0.01f,
              "pan: space drag moves camera without starting a block drag");

        // Wheel and shift+wheel pan both axes at the event position.
        SDL_Event wev{};
        wev.type = SDL_MOUSEWHEEL;
        wev.wheel.y = 1;
        wev.wheel.mouseX = (int)cx;
        wev.wheel.mouseY = (int)cy;
        const float wy0 = panY;
        check(handleEvent(wev), "pan: wheel accepted");
        check(std::fabs(panY - (wy0 + 30.f)) < 0.01f,
              "pan: wheel moves the camera vertically");
        wev.wheel.y = 0;
        wev.wheel.x = 1;
        SDL_SetModState(KMOD_SHIFT);
        const float wx0 = panX;
        check(handleEvent(wev), "pan: horizontal wheel accepted");
        SDL_SetModState(KMOD_NONE);
        check(std::fabs(panX - (wx0 + 30.f)) < 0.01f,
              "pan: shift + wheel moves the camera horizontally");

        drag = Drag{};
        t->blocks = &sandbox;
        wsDirty_ = true;
        rebuildWs();
    }

    // Restore before sandbox destruction; do not leave a dangling selected model.
    drag = Drag{};
    clearHistory();
    t->blocks = savedBlocks;
    tab = savedTab;
    catSel = savedCat;
    panX = savedPanX;
    panY = savedPanY;
    zoom = savedZoom;
    selBlock.clear();
    wsDirty_ = flyDirty_ = true;
    rebuildWs();
    rebuildFlyout();
}
