#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <chrono>

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;

// ───────────────────────── Buffer state (one level at a time) ─────────────────────────

struct BufferSlot {
	bool pending = false;      // a click is waiting for landing / an orb
	bool held = false;         // button is physically down
	bool releaseNext = false;  // release a click we injected
	Clock::time_point pressTime;
};

static BufferSlot g_slot[2];   // 0 = player 1, 1 = player 2
static bool g_injecting = false;

static void resetBufferState() {
	g_slot[0] = BufferSlot{};
	g_slot[1] = BufferSlot{};
	g_injecting = false;
}

static bool touchingRing(PlayerObject* p) {
	return p && p->m_touchingRings && p->m_touchingRings->count() > 0;
}

// Cube / ball / robot / spider need to be on the ground to act.
// Ship / UFO / wave / swing can be clicked any time, so landings don't need buffering.
static bool isGroundMode(PlayerObject* p) {
	return !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
}

// ───────────────────────── Settings popup (Geode v5 Popup) ─────────────────────────

static constexpr char const* kKeys[3] = {"enabled", "buffer-ground", "buffer-orbs"};

class BufferPopup : public Popup {
public:
	static BufferPopup* create() {
		auto ret = new BufferPopup();
		if (ret->init()) {
			ret->autorelease();
			return ret;
		}
		delete ret;
		return nullptr;
	}

protected:
	bool init() {
		if (!Popup::init(260.f, 190.f)) return false;
		this->setTitle("Input Buffer");

		addRow("Buffer enabled", 0, 30.f);
		addRow("Buffer landings", 1, -5.f);
		addRow("Buffer orbs", 2, -40.f);
		return true;
	}

	void addRow(char const* text, int index, float y) {
		auto label = CCLabelBMFont::create(text, "bigFont.fnt");
		label->setScale(0.5f);
		label->setAnchorPoint({0.f, 0.5f});
		m_mainLayer->addChildAtPosition(label, Anchor::Center, {-110.f, y});

		auto toggler = CCMenuItemToggler::createWithStandardSprites(
			this, menu_selector(BufferPopup::onToggle), 0.8f
		);
		toggler->setTag(index);
		toggler->toggle(Mod::get()->getSettingValue<bool>(kKeys[index]));
		m_buttonMenu->addChildAtPosition(toggler, Anchor::Center, {95.f, y});
	}

	void onToggle(CCObject* sender) {
		auto toggler = static_cast<CCMenuItemToggler*>(sender);
		int index = toggler->getTag();
		if (index < 0 || index > 2) return;
		// isToggled() still holds the state from *before* this tap.
		Mod::get()->setSettingValue<bool>(kKeys[index], !toggler->isToggled());
		if (index == 0) resetBufferState();
	}
};

// ───────────────────────── Buttons in main menu & pause menu ─────────────────────────

static CCMenuItemSpriteExtra* makeBufferButton(CCObject* target, SEL_MenuHandler handler) {
	auto label = CCLabelBMFont::create("IB", "bigFont.fnt");
	label->setScale(0.6f);
	auto spr = CircleButtonSprite::create(label);
	return CCMenuItemSpriteExtra::create(spr, target, handler);
}

class $modify(BufferMenuLayer, MenuLayer) {
	bool init() {
		if (!MenuLayer::init()) return false;

		auto btn = makeBufferButton(this, menu_selector(BufferMenuLayer::onBuffer));
		btn->setID("input-buffer-button"_spr);

		if (auto menu = this->getChildByID("bottom-menu")) {
			menu->addChild(btn);
			menu->updateLayout();
		} else {
			auto menu = CCMenu::create();
			menu->addChild(btn);
			menu->setPosition({40.f, 40.f});
			this->addChild(menu);
		}
		return true;
	}

	void onBuffer(CCObject*) {
		BufferPopup::create()->show();
	}
};

class $modify(BufferPauseLayer, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();

		auto btn = makeBufferButton(this, menu_selector(BufferPauseLayer::onBuffer));
		btn->setID("input-buffer-button"_spr);

		if (auto menu = this->getChildByID("left-button-menu")) {
			menu->addChild(btn);
			menu->updateLayout();
		} else {
			auto menu = CCMenu::create();
			menu->addChild(btn);
			auto size = CCDirector::get()->getWinSize();
			menu->setPosition({40.f, size.height / 2.f});
			this->addChild(menu);
		}
	}

	void onBuffer(CCObject*) {
		BufferPopup::create()->show();
	}
};

// ───────────────────────── Clear stale state on (re)start ─────────────────────────

class $modify(BufferPlayLayer, PlayLayer) {
	void resetLevel() {
		PlayLayer::resetLevel();
		resetBufferState();
	}
};

// ───────────────────────── The buffer itself ─────────────────────────

class $modify(BufferGameLayer, GJBaseGameLayer) {
	bool inPlayLayer() {
		return static_cast<GJBaseGameLayer*>(PlayLayer::get()) == static_cast<GJBaseGameLayer*>(this);
	}

	PlayerObject* playerFor(int i) {
		return i == 0 ? m_player1 : m_player2;
	}

	// Inject a click. rePress = the real button is still down, so release first
	// to make the game see a fresh press.
	void fire(int i, bool rePress) {
		bool isP1 = (i == 0);
		auto& s = g_slot[i];
		s.pending = false;
		g_injecting = true;
		if (rePress) GJBaseGameLayer::handleButton(false, 1, isP1);
		GJBaseGameLayer::handleButton(true, 1, isP1);
		g_injecting = false;
		s.releaseNext = !rePress;
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		if (button != 1 || g_injecting || !inPlayLayer()
			|| !Mod::get()->getSettingValue<bool>("enabled")) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		int i = isPlayer1 ? 0 : 1;
		auto& s = g_slot[i];
		s.held = down;

		if (down) {
			auto player = playerFor(i);
			// Only buffer clicks that can't do anything right now. Clicks on the
			// ground, or while already touching an orb, are handled by the game.
			if (player && !player->m_isOnGround && !touchingRing(player)) {
				s.pending = true;
				s.pressTime = Clock::now();
			}
		}
		// A release does NOT cancel the buffer - that's the whole point.

		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	void processCommands(float dt) {
		GJBaseGameLayer::processCommands(dt);

		if (!inPlayLayer() || !Mod::get()->getSettingValue<bool>("enabled")) return;

		auto windowMs = Mod::get()->getSettingValue<int64_t>("buffer-ms");
		bool orbs = Mod::get()->getSettingValue<bool>("buffer-orbs");
		bool ground = Mod::get()->getSettingValue<bool>("buffer-ground");

		for (int i = 0; i < 2; i++) {
			bool isP1 = (i == 0);
			auto& s = g_slot[i];

			if (s.releaseNext) {
				s.releaseNext = false;
				g_injecting = true;
				GJBaseGameLayer::handleButton(false, 1, isP1);
				g_injecting = false;
			}

			if (!s.pending) continue;

			auto player = playerFor(i);
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				Clock::now() - s.pressTime).count();

			if (!player || player->m_isDead || elapsed > windowMs) {
				s.pending = false;
				continue;
			}

			if (orbs && touchingRing(player)) {
				fire(i, s.held);
			} else if (ground && player->m_isOnGround && isGroundMode(player)) {
				if (s.held) s.pending = false; // game already re-jumps on landing while held
				else fire(i, false);
			}
		}
	}
};
