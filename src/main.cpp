// Click Buffer - Geometry Dash 2.2081 / Geode 5.11.2
//
// If you click too early (while still in the air), the click is remembered and
// replayed on the first physics step where you are back on the ground, as long as
// that happens within N steps (N = 0..30, set in the pause menu).
//
// Covers cube, ball, robot and spider, since those are the modes where a click
// does nothing in mid-air. Ship, UFO, wave and swing react to clicks anywhere, so
// they are left alone. Orbs and pads are not touched.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/UILayer.hpp>
#include <Geode/ui/Popup.hpp>

#include <algorithm>
#include <cmath>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Easy things to tweak
// ---------------------------------------------------------------------------
namespace {
    constexpr int   MAX_FRAMES      = 30;     // upper limit of the buffer
    constexpr float INDICATOR_SIZE  = 5.f;    // radius of the red face (GD units, very small)
    constexpr float INDICATOR_MARGIN = 10.f;  // distance from the screen edge
    constexpr bool  INDICATOR_RIGHT = false;  // false = top-left, true = top-right
    constexpr float PAUSE_BTN_X     = 70.f;   // pause menu button position (from bottom-left)
    constexpr float PAUSE_BTN_Y     = 24.f;
    constexpr float PI_F            = 3.14159265f;
}

// ---------------------------------------------------------------------------
// Settings helpers (the keys live in mod.json)
// ---------------------------------------------------------------------------
static bool bufferEnabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}

static bool indicatorWanted() {
    return Mod::get()->getSettingValue<bool>("show-indicator");
}

static int bufferFrames() {
    auto v = Mod::get()->getSettingValue<int64_t>("buffer-frames");
    return static_cast<int>(std::clamp<int64_t>(v, 0, MAX_FRAMES));
}

// ---------------------------------------------------------------------------
// Buffer state (one gameplay layer exists at a time, so plain globals are fine)
// ---------------------------------------------------------------------------
namespace {
    struct Slot {
        int  mask      = 0;      // bit0 = player 1 is waiting to land, bit1 = player 2
        int  left      = 0;      // physics steps this click may still wait
        int  age       = 0;      // safety net: calls since the click, so it can never get stuck
        int  heldSteps = 0;      // how long the real click was held while we were waiting
        int  releaseIn = 0;      // steps until we let go of the replayed click
        bool held      = false;  // is the real button down right now
    };

    struct BufferState {
        Slot slot[2];            // [0] = player 1 button, [1] = player 2 button
        bool injecting = false;  // true while we are sending our own click

        bool busy() const {
            return slot[0].mask || slot[1].mask || slot[0].releaseIn || slot[1].releaseIn;
        }
    };

    BufferState g_buf;
}

// Modes where a click only does something while standing on the floor/ceiling.
static bool needsGround(PlayerObject* p) {
    return !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
}

static bool waitingForGround(PlayerObject* p) {
    return p && !p->m_isDead && needsGround(p) && !p->m_isOnGround;
}

static bool touchingGround(PlayerObject* p) {
    return p && !p->m_isDead && needsGround(p) && p->m_isOnGround;
}

// ---------------------------------------------------------------------------
// The little red face (drawn with code, so no texture names can go wrong)
// ---------------------------------------------------------------------------
static CCDrawNode* makeFace(float r) {
    auto node = CCDrawNode::create();

    ccColor4F red  = { 0.92f, 0.10f, 0.10f, 1.f };
    ccColor4F dark = { 0.20f, 0.00f, 0.00f, 1.f };

    node->drawDot(ccp(0, 0), r, red);

    // eyes
    node->drawDot(ccp(-r * 0.35f, r * 0.20f), r * 0.14f, dark);
    node->drawDot(ccp( r * 0.35f, r * 0.20f), r * 0.14f, dark);

    // smile (lower arc)
    constexpr int segments = 8;
    CCPoint prev = ccp(0, 0);
    for (int i = 0; i <= segments; ++i) {
        float a = (200.f + 140.f * static_cast<float>(i) / segments) * PI_F / 180.f;
        CCPoint p = ccp(std::cos(a) * r * 0.5f, std::sin(a) * r * 0.5f - r * 0.05f);
        if (i > 0) node->drawSegment(prev, p, r * 0.07f, dark);
        prev = p;
    }
    return node;
}

class BufferIndicator : public CCNode {
protected:
    float m_timer = 0.f;

    bool init() override {
        if (!CCNode::init()) return false;
        this->addChild(makeFace(INDICATOR_SIZE));
        this->refresh();
        this->scheduleUpdate();
        return true;
    }

    void refresh() {
        this->setVisible(bufferEnabled() && indicatorWanted());
    }

    void update(float dt) override {
        m_timer += dt;
        if (m_timer >= 0.25f) {
            m_timer = 0.f;
            this->refresh();
        }
    }

public:
    static BufferIndicator* create() {
        auto ret = new BufferIndicator();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Core logic
// ---------------------------------------------------------------------------
class $modify(BufferGameLayer, GJBaseGameLayer) {

    // Every button press/release goes through here.
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!g_buf.injecting && button == static_cast<int>(PlayerButton::Jump) && bufferEnabled()) {
            this->noteRealClick(down, isPlayer1);
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Runs once per physics step (and for half ticks, which we ignore).
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_buf.busy()) {
            this->stepBuffer(isHalfTick);
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    // A real click from the player. Decide if it needs to be remembered.
    void noteRealClick(bool down, bool isPlayer1) {
        Slot& s = g_buf.slot[isPlayer1 ? 0 : 1];

        if (!down) {
            s.held = false;
            return;
        }

        s.held      = true;
        s.releaseIn = 0;   // a new real click cancels any pending replayed release
        s.heldSteps = 0;
        s.mask      = 0;

        int frames = bufferFrames();
        if (frames <= 0) return;

        // In classic dual mode one click controls both players.
        bool classicDual = m_gameState.m_isDualMode && !(m_player1 && m_player1->m_isPlatformer);

        if (isPlayer1) {
            if (waitingForGround(m_player1)) s.mask |= 1;
            if (classicDual && waitingForGround(m_player2)) s.mask |= 2;
        }
        else if (waitingForGround(m_player2)) {
            s.mask |= 2;
        }

        if (s.mask) {
            s.left = frames;
            s.age  = 0;
        }
    }

    // Called at the start of a physics step, so a replayed click lands in that step.
    void stepBuffer(bool isHalfTick) {
        for (int i = 0; i < 2; ++i) {
            Slot& s = g_buf.slot[i];
            bool isP1 = (i == 0);

            if (s.mask) s.age++;
            if (isHalfTick) continue;

            // let go of a replayed click once the original hold time is over
            if (s.releaseIn > 0 && --s.releaseIn == 0 && !s.held) {
                this->replayClick(false, isP1);
            }

            if (!s.mask) continue;

            // dead players don't need their click any more
            if (m_player1 && m_player1->m_isDead) s.mask &= ~1;
            if (m_player2 && m_player2->m_isDead) s.mask &= ~2;

            // safety net
            if (s.age > MAX_FRAMES * 8) s.mask = 0;
            if (!s.mask) { s.heldSteps = 0; continue; }

            if (s.held) s.heldSteps++;

            int landed = 0;
            if ((s.mask & 1) && touchingGround(m_player1)) landed |= 1;
            if ((s.mask & 2) && touchingGround(m_player2)) landed |= 2;

            if (landed) {
                s.mask &= ~landed;
                this->replayClick(true, isP1);
                // if the real button was already let go, let go of ours after the same hold time
                s.releaseIn = s.held ? 0 : std::max(2, s.heldSteps);
            }

            if (s.mask && --s.left <= 0) s.mask = 0;
            if (!s.mask) s.heldSteps = 0;
        }
    }

    // Send our own click through the normal path.
    void replayClick(bool down, bool isPlayer1) {
        g_buf.injecting = true;
        this->handleButton(down, static_cast<int>(PlayerButton::Jump), isPlayer1);
        g_buf.injecting = false;
    }
};

// Forget everything when a level restarts or is left.
class $modify(BufferPlayLayer, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        g_buf = BufferState{};
    }

    void onQuit() {
        PlayLayer::onQuit();
        g_buf = BufferState{};
    }
};

// ---------------------------------------------------------------------------
// On-screen indicator
// ---------------------------------------------------------------------------
class $modify(BufferUILayer, UILayer) {
    bool init(GJBaseGameLayer* layer) {
        if (!UILayer::init(layer)) return false;

        auto win = CCDirector::get()->getWinSize();
        auto face = BufferIndicator::create();
        if (face) {
            face->setID("click-buffer-indicator"_spr);
            float x = INDICATOR_RIGHT ? win.width - INDICATOR_MARGIN : INDICATOR_MARGIN;
            face->setPosition(ccp(x, win.height - INDICATOR_MARGIN));
            this->addChild(face, 100);
        }
        return true;
    }
};

// ---------------------------------------------------------------------------
// Pause menu popup
// ---------------------------------------------------------------------------
class BufferPopup : public Popup {
protected:
    CCLabelBMFont* m_frameLabel = nullptr;

    bool setup() {
        if (!Popup::init(300.f, 200.f)) return false;

        this->setTitle("Click Buffer");

        this->addToggleRow("Enabled", 1, bufferEnabled(), 52.f);
        this->addToggleRow("Show red face", 2, indicatorWanted(), 22.f);

        auto head = CCLabelBMFont::create("Replay early clicks up to", "bigFont.fnt");
        head->setScale(.42f);
        m_mainLayer->addChildAtPosition(head, Anchor::Center, ccp(0, -8));

        m_frameLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_frameLabel->setScale(.8f);
        m_mainLayer->addChildAtPosition(m_frameLabel, Anchor::Center, ccp(0, -38));

        struct Step { char const* text; int delta; float x; };
        for (auto const& st : { Step{"-5", -5, -105.f}, Step{"-1", -1, -62.f},
                                Step{"+1", 1, 62.f},    Step{"+5", 5, 105.f} }) {
            auto spr = ButtonSprite::create(st.text, "bigFont.fnt", "GJ_button_01.png", .8f);
            spr->setScale(.7f);
            auto btn = CCMenuItemSpriteExtra::create(
                spr, this, menu_selector(BufferPopup::onStep)
            );
            btn->setTag(st.delta);
            m_buttonMenu->addChildAtPosition(btn, Anchor::Center, ccp(st.x, -38));
        }

        auto note = CCLabelBMFont::create("0 = off, max 30 frames", "chatFont.fnt");
        note->setScale(.6f);
        note->setOpacity(160);
        m_mainLayer->addChildAtPosition(note, Anchor::Center, ccp(0, -72));

        this->refreshLabel();
        return true;
    }

    void addToggleRow(char const* text, int tag, bool on, float y) {
        auto label = CCLabelBMFont::create(text, "bigFont.fnt");
        label->setScale(.5f);
        m_mainLayer->addChildAtPosition(label, Anchor::Center, ccp(-120, y), ccp(0, .5f));

        auto toggle = CCMenuItemToggler::createWithStandardSprites(
            this, menu_selector(BufferPopup::onToggle), .65f
        );
        toggle->setTag(tag);
        toggle->toggle(on);
        m_buttonMenu->addChildAtPosition(toggle, Anchor::Center, ccp(115, y));
    }

    void onToggle(CCObject* sender) {
        auto toggler = static_cast<CCMenuItemToggler*>(sender);
        // GD calls this before flipping, so isToggled() is still the OLD state
        bool nowOn = !toggler->isToggled();
        if (toggler->getTag() == 1) {
            Mod::get()->setSettingValue<bool>("enabled", nowOn);
        }
        else {
            Mod::get()->setSettingValue<bool>("show-indicator", nowOn);
        }
    }

    void onStep(CCObject* sender) {
        int v = bufferFrames() + static_cast<CCNode*>(sender)->getTag();
        v = std::clamp(v, 0, MAX_FRAMES);
        Mod::get()->setSettingValue<int64_t>("buffer-frames", v);
        this->refreshLabel();
    }

    void refreshLabel() {
        int n = bufferFrames();
        if (n == 0) {
            m_frameLabel->setString("0 frames (off)");
        }
        else {
            m_frameLabel->setString(fmt::format("{} frame{}", n, n == 1 ? "" : "s").c_str());
        }
    }

public:
    static BufferPopup* create() {
        auto ret = new BufferPopup();
        if (ret->setup()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

class $modify(BufferPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto spr = ButtonSprite::create("Click Buffer", "bigFont.fnt", "GJ_button_01.png", .8f);
        spr->setScale(.55f);
        auto btn = CCMenuItemSpriteExtra::create(
            spr, this, menu_selector(BufferPauseLayer::onClickBuffer)
        );

        auto menu = CCMenu::create();
        menu->setID("click-buffer-menu"_spr);
        menu->setPosition(ccp(PAUSE_BTN_X, PAUSE_BTN_Y));
        menu->addChild(btn);
        this->addChild(menu);
    }

    void onClickBuffer(CCObject*) {
        if (auto popup = BufferPopup::create()) {
            popup->show();
        }
    }
};