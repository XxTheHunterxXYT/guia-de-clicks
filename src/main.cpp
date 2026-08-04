#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/utils/file.hpp>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <filesystem>
#include <vector>
#include <cmath>
#include <ctime>

using namespace geode::prelude;

struct InputEvent {
	bool down;
	int button;
	float percent;
	float posX;
	float posY;
};

struct GuideHold {
	float startX = 0.f;
	float endX = 0.f;
	float y = 0.f;
	bool isHold = false;
	bool resolved = false;
	CCDrawNode* markNode = nullptr;
	CCDrawNode* ringNode = nullptr;
};

static std::vector<InputEvent> currentRecording;
static std::vector<InputEvent> loadedGuide;
static std::vector<GuideHold> guideHolds;
static std::string currentLevelID;
static std::string pendingRecordingName;
static float pendingDownX = -1.f;
static size_t nextHoldIndex = 0;
static float lastPosX = -1.f;
static float frameStep = 5.77f;
static bool isRecording = false;
static bool isReplaying = false;
static bool isBotMode = false;
static PauseLayer* activePauseLayer = nullptr;

static int statPerfects = 0;
static int statGoods = 0;
static int statMisses = 0;

constexpr float RING_MAX_RADIUS = 55.f;
constexpr float RING_MIN_RADIUS = 10.f;
constexpr float APPROACH_DISTANCE = 320.f;
constexpr float FADE_START = 500.f;
constexpr float MATCH_WINDOW = 150.f;
constexpr float MIN_WIDTH = 8.f;
constexpr float HOLD_THRESHOLD = 12.f;
constexpr float LINE_HEIGHT = 600.f;

// ---------- Archivos ----------

std::vector<InputEvent> loadRecording(std::filesystem::path const& path) {
	std::vector<InputEvent> result;
	auto readRes = file::readString(path);
	if (!readRes) return result;

	auto jsonRes = matjson::parse(readRes.unwrap());
	if (!jsonRes) return result;

	for (auto& item : jsonRes.unwrap().asArray().unwrap()) {
		InputEvent ev{};
		ev.down = item["down"].asBool().unwrapOr(true);
		ev.button = (int)item["button"].asInt().unwrapOr(1);
		ev.percent = (float)item["percent"].asDouble().unwrapOr(0.0);
		ev.posX = (float)item["posX"].asDouble().unwrapOr(0.0);
		ev.posY = (float)item["posY"].asDouble().unwrapOr(0.0);
		result.push_back(ev);
	}
	return result;
}

void saveRecordingToFile(std::string const& levelID, std::string const& name, std::vector<InputEvent> const& data) {
	auto dir = Mod::get()->getSaveDir() / "recordings" / levelID;
	std::filesystem::create_directories(dir);

	auto path = dir / (name + ".json");
	matjson::Value arr = matjson::Value::array();

	for (auto& ev : data) {
		matjson::Value obj = matjson::Value::object();
		obj["down"] = ev.down;
		obj["button"] = ev.button;
		obj["percent"] = (double)ev.percent;
		obj["posX"] = (double)ev.posX;
		obj["posY"] = (double)ev.posY;
		arr.push(obj);
	}

	file::writeString(path, arr.dump());
	log::info("Grabacion guardada: {} eventos en {}", data.size(), path.string());
}

std::vector<std::string> listRecordings(std::string const& levelID) {
	std::vector<std::string> names;
	auto dir = Mod::get()->getSaveDir() / "recordings" / levelID;
	if (!std::filesystem::exists(dir)) return names;
	for (auto& entry : std::filesystem::directory_iterator(dir)) {
		if (entry.path().extension() == ".json") {
			names.push_back(entry.path().stem().string());
		}
	}
	return names;
}

// ---------- Dibujo ----------

std::vector<CCPoint> circlePoints(CCPoint center, float radius, int segments = 32) {
	std::vector<CCPoint> pts;
	for (int i = 0; i < segments; i++) {
		float angle = (float)i / segments * 6.2831853f;
		pts.push_back({ center.x + cosf(angle) * radius, center.y + sinf(angle) * radius });
	}
	return pts;
}

void buildGuideHolds(PlayLayer* pl) {
	for (auto& hold : guideHolds) {
		if (hold.markNode) hold.markNode->removeFromParent();
		if (hold.ringNode) hold.ringNode->removeFromParent();
	}
	guideHolds.clear();
	nextHoldIndex = 0;

	if (!pl) return;

	float pendingX = -1.f;
	float pendingY = 0.f;

	for (auto& ev : loadedGuide) {
		if (ev.button != 1) continue;
		if (ev.down) {
			pendingX = ev.posX;
			pendingY = ev.posY;
		}
		else if (pendingX >= 0.f) {
			GuideHold hold;
			hold.startX = pendingX;
			hold.endX = std::max(ev.posX, pendingX + MIN_WIDTH);
			hold.y = pendingY;
			hold.isHold = (hold.endX - hold.startX) > HOLD_THRESHOLD;

			hold.markNode = CCDrawNode::create();
			pl->m_objectLayer->addChild(hold.markNode, 100);

			hold.ringNode = CCDrawNode::create();
			pl->m_objectLayer->addChild(hold.ringNode, 101);

			guideHolds.push_back(hold);
			pendingX = -1.f;
		}
	}
}

void fadeOutAndClear(CCDrawNode* node) {
	if (!node) return;
	node->stopAllActions();
	node->runAction(CCSequence::create(
		CCDelayTime::create(0.25f),
		CCCallFunc::create(node, callfunc_selector(CCDrawNode::clear)),
		nullptr
	));
}

void spawnOffsetLabel(PlayLayer* pl, CCPoint pos, int frameOffset, bool isMiss) {
	if (isBotMode) return;

	std::string text;
	ccColor3B color;

	if (isMiss) {
		text = "MISS";
		color = ccc3(255, 40, 40);
		statMisses++;
	}
	else if (frameOffset == 0) {
		text = "PERFECT";
		color = ccc3(80, 255, 80);
		statPerfects++;
	}
	else {
		text = (frameOffset > 0 ? "+" : "") + std::to_string(frameOffset);
		int absOffset = std::abs(frameOffset);
		if (absOffset <= 2) { color = ccc3(80, 255, 80); statGoods++; }
		else if (absOffset <= 5) { color = ccc3(255, 220, 60); statGoods++; }
		else { color = ccc3(255, 80, 80); statMisses++; }
	}

	auto label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
	label->setScale(0.4f);
	label->setColor(color);
	label->setPosition(pos + CCPoint{ 0, 30 });
	pl->m_objectLayer->addChild(label, 103);

	label->runAction(CCSequence::create(
		CCSpawn::create(
			CCMoveBy::create(0.6f, { 0, 40 }),
			CCFadeOut::create(0.6f),
			nullptr
		),
		CCCallFunc::create(label, callfunc_selector(CCNode::removeFromParent)),
		nullptr
	));
}

void drawMark(GuideHold& hold, float alpha) {
	if (isBotMode) return;

	hold.markNode->clear();
	float lineYTop = hold.y + LINE_HEIGHT / 2;
	float lineYBot = hold.y - LINE_HEIGHT / 2;

	if (hold.isHold) {
		CCPoint rectPts[4] = {
			{hold.startX, lineYBot},
			{hold.endX,   lineYBot},
			{hold.endX,   lineYTop},
			{hold.startX, lineYTop},
		};
		auto holdFill = ccc4f(0.2f, 0.9f, 0.3f, 0.15f * alpha);
		auto holdBorder = ccc4f(1.0f, 1.0f, 1.0f, 0.7f * alpha);
		hold.markNode->drawPolygon(rectPts, 4, holdFill, 1.5f, holdBorder);
	}
	else {
		auto clickColor = ccc4f(0.9f, 0.9f, 0.9f, 0.8f * alpha);
		hold.markNode->drawSegment({ hold.startX, lineYBot }, { hold.startX, lineYTop }, 2.f, clickColor);
	}
}

// ---------- Popups ----------

class RecordNamePopup : public geode::Popup {
protected:
	TextInput* m_input = nullptr;

	bool init() {
		if (!Popup::init(280.f, 150.f)) return false;
		this->setTitle("Grabar guia");

		m_input = TextInput::create(220.f, "Nombre de la grabacion");
		m_mainLayer->addChildAtPosition(m_input, Anchor::Center, { 0, 10 });

		auto startBtn = CCMenuItemSpriteExtra::create(
			ButtonSprite::create("Empezar"),
			this,
			menu_selector(RecordNamePopup::onStart)
		);
		auto menu = CCMenu::create();
		menu->addChild(startBtn);
		m_mainLayer->addChildAtPosition(menu, Anchor::Center, { 0, -35 });
		return true;
	}

	void onStart(CCObject*) {
		auto name = m_input->getString();
		if (name.empty()) name = "grabacion";
		pendingRecordingName = name;
		isRecording = true;
		isReplaying = false;
		isBotMode = false;
		currentRecording.clear();
		loadedGuide.clear();
		buildGuideHolds(PlayLayer::get());

		this->onClose(nullptr);
		if (activePauseLayer) {
			activePauseLayer->onResume(nullptr);
			activePauseLayer = nullptr;
		}
	}

public:
	static RecordNamePopup* create() {
		auto ret = new RecordNamePopup();
		if (ret->init()) { ret->autorelease(); return ret; }
		delete ret;
		return nullptr;
	}
};

class ReplayListPopup : public geode::Popup {
protected:
	std::string m_levelID;

	bool init(std::string const& levelID) {
		if (!Popup::init(300.f, 240.f)) return false;
		m_levelID = levelID;
		this->setTitle("Elige grabacion / Bot");

		auto files = listRecordings(levelID);
		if (files.empty()) {
			auto label = CCLabelBMFont::create("Coloca tus JSON en recordings/", "chatFont.fnt");
			label->setScale(0.5f);
			m_mainLayer->addChildAtPosition(label, Anchor::Center);
		}
		else {
			auto menu = CCMenu::create();
			menu->setLayout(ColumnLayout::create()->setGap(6.f));

			for (auto& name : files) {
				auto rowMenu = CCMenu::create();
				rowMenu->setLayout(RowLayout::create()->setGap(6.f));

				auto btnGuide = CCMenuItemSpriteExtra::create(
					ButtonSprite::create((name + " [Guia]").c_str()),
					this,
					menu_selector(ReplayListPopup::onPickGuide)
				);
				btnGuide->setUserObject(CCString::create(name));
				rowMenu->addChild(btnGuide);

				auto btnBot = CCMenuItemSpriteExtra::create(
					ButtonSprite::create("Bot"),
					this,
					menu_selector(ReplayListPopup::onPickBot)
				);
				btnBot->setUserObject(CCString::create(name));
				rowMenu->addChild(btnBot);

				auto delSpr = CircleButtonSprite::create(CCSprite::createWithSpriteFrameName("GJ_deleteBtn_001.png"), CircleBaseColor::Red, CircleBaseSize::Small);
				delSpr->setScale(0.6f);
				auto delBtn = CCMenuItemSpriteExtra::create(delSpr, this, menu_selector(ReplayListPopup::onDelete));
				delBtn->setUserObject(CCString::create(name));
				rowMenu->addChild(delBtn);

				rowMenu->updateLayout();
				menu->addChild(rowMenu);
			}
			menu->updateLayout();
			m_mainLayer->addChildAtPosition(menu, Anchor::Center);
		}
		return true;
	}

	void loadSelected(CCObject* sender, bool botMode) {
		auto btn = static_cast<CCMenuItemSpriteExtra*>(sender);
		auto nameObj = static_cast<CCString*>(btn->getUserObject());
		std::string name = nameObj->getCString();

		auto path = Mod::get()->getSaveDir() / "recordings" / m_levelID / (name + ".json");
		loadedGuide = loadRecording(path);

		auto playLayer = PlayLayer::get();
		buildGuideHolds(playLayer);

		isReplaying = true;
		isBotMode = botMode;
		statPerfects = 0; statGoods = 0; statMisses = 0;

		this->onClose(nullptr);
		if (activePauseLayer) {
			activePauseLayer->onResume(nullptr);
			activePauseLayer = nullptr;
		}
	}

	void onPickGuide(CCObject* sender) { loadSelected(sender, false); }
	void onPickBot(CCObject* sender) { loadSelected(sender, true); }

	void onDelete(CCObject* sender) {
		auto btn = static_cast<CCMenuItemSpriteExtra*>(sender);
		auto nameObj = static_cast<CCString*>(btn->getUserObject());
		std::filesystem::remove(Mod::get()->getSaveDir() / "recordings" / m_levelID / (nameObj->getCString() + std::string(".json")));
		this->onClose(nullptr);
		ReplayListPopup::create(m_levelID)->show();
	}

public:
	static ReplayListPopup* create(std::string const& levelID) {
		auto ret = new ReplayListPopup();
		if (ret->init(levelID)) { ret->autorelease(); return ret; }
		delete ret;
		return nullptr;
	}
};

// ---------- Hooks del juego ----------

class $modify(MyGameLayer, GJBaseGameLayer) {
	void handleButton(bool down, int button, bool isPlayer1) {
		GJBaseGameLayer::handleButton(down, button, isPlayer1);

		auto playLayer = typeinfo_cast<PlayLayer*>(this);
		if (!playLayer || !playLayer->m_player1 || !isPlayer1) return;

		float posX = playLayer->m_player1->getPositionX();
		float posY = playLayer->m_player1->getPositionY();

		if (isRecording) {
			currentRecording.push_back({ down, button, playLayer->getCurrentPercent(), posX, posY });
		}

		if (!isReplaying || isBotMode || button != 1) return;

		if (down) {
			pendingDownX = posX;
		}
		else if (pendingDownX >= 0.f) {
			float startX = pendingDownX;
			pendingDownX = -1.f;

			if (nextHoldIndex < guideHolds.size()) {
				auto& hold = guideHolds[nextHoldIndex];
				float dist = startX - hold.startX;
				if (std::abs(dist) <= MATCH_WINDOW) {
					hold.resolved = true;
					fadeOutAndClear(hold.markNode);
					fadeOutAndClear(hold.ringNode);
					float step = (frameStep != 0.f) ? frameStep : 5.77f;
					spawnOffsetLabel(playLayer, { hold.startX, hold.y }, (int)std::round(dist / step), false);
					nextHoldIndex++;
				}
			}
		}
	}

	void update(float dt) {
		GJBaseGameLayer::update(dt);

		auto playLayer = typeinfo_cast<PlayLayer*>(this);
		if (!playLayer || !playLayer->m_player1) return;

		float posX = playLayer->m_player1->getPositionX();

		if (isReplaying && isBotMode) {
			while (nextHoldIndex < loadedGuide.size()) {
				auto& ev = loadedGuide[nextHoldIndex];
				if (posX >= ev.posX) {
					GJBaseGameLayer::handleButton(ev.down, ev.button, true);
					nextHoldIndex++;
				}
				else {
					break;
				}
			}
			return;
		}

		if (!isReplaying) return;

		if (lastPosX >= 0.f) {
			float step = posX - lastPosX;
			if (step > 0.01f) frameStep = step;
		}
		lastPosX = posX;

		while (nextHoldIndex < guideHolds.size() && posX > guideHolds[nextHoldIndex].startX + MATCH_WINDOW) {
			auto& hold = guideHolds[nextHoldIndex];
			fadeOutAndClear(hold.markNode);
			fadeOutAndClear(hold.ringNode);
			if (!hold.resolved) {
				hold.resolved = true;
				spawnOffsetLabel(playLayer, { hold.startX, hold.y }, 0, true);
			}
			nextHoldIndex++;
		}

		for (size_t i = nextHoldIndex; i < guideHolds.size(); i++) {
			auto& hold = guideHolds[i];
			float distanceRemaining = hold.startX - posX;

			if (distanceRemaining > FADE_START) {
				hold.markNode->clear();
				hold.ringNode->clear();
				continue;
			}

			float alpha = std::clamp(distanceRemaining > APPROACH_DISTANCE ? 1.f - (distanceRemaining - APPROACH_DISTANCE) / (FADE_START - APPROACH_DISTANCE) : 1.f, 0.f, 1.f);
			drawMark(hold, alpha);

			hold.ringNode->clear();
			if (i == nextHoldIndex && distanceRemaining <= APPROACH_DISTANCE) {
				float t = std::clamp(distanceRemaining / APPROACH_DISTANCE, 0.f, 1.f);
				float radius = RING_MIN_RADIUS + (RING_MAX_RADIUS - RING_MIN_RADIUS) * t;
				auto outerPts = circlePoints({ posX, playLayer->m_player1->getPositionY() }, radius, 40);
				auto ringColor = ccc4f(0.2f, 1.0f, 0.3f, 1.0f - (t * 0.3f));
				hold.ringNode->drawPolygon(outerPts.data(), (unsigned int)outerPts.size(), ccc4f(0, 0, 0, 0), 2.5f, ringColor);
			}
		}
	}
};

class $modify(MyPlayLayer, PlayLayer) {
	bool init(GJGameLevel * level, bool useReplay, bool dontCreateObjects) {
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

		currentRecording.clear();
		pendingDownX = -1.f;
		lastPosX = -1.f;
		isRecording = false;
		isReplaying = false;
		isBotMode = false;
		currentLevelID = std::to_string(level->m_levelID.value());
		loadedGuide.clear();
		buildGuideHolds(this);
		return true;
	}

	void resetLevel() {
		PlayLayer::resetLevel();
		pendingDownX = -1.f;
		lastPosX = -1.f;
		if (isRecording) currentRecording.clear();
		if (isReplaying) {
			for (auto& hold : guideHolds) {
				if (hold.markNode) { hold.markNode->stopAllActions(); hold.markNode->clear(); }
				if (hold.ringNode) { hold.ringNode->stopAllActions(); hold.ringNode->clear(); }
				hold.resolved = false;
			}
			nextHoldIndex = 0;
			statPerfects = 0; statGoods = 0; statMisses = 0;
		}
	}

	void levelComplete() {
		PlayLayer::levelComplete();
		if (isRecording) {
			isRecording = false;
			std::string name = pendingRecordingName;
			if (name.empty()) name = "grabacion_" + std::to_string((long)time(nullptr));
			saveRecordingToFile(currentLevelID, name, currentRecording);
			Notification::create("Grabacion completa", NotificationIcon::Success)->show();
		}
		else if (isReplaying && !isBotMode) {
			std::string summary = fmt::format("Stats -> P: {} | G: {} | M: {}", statPerfects, statGoods, statMisses);
			Notification::create(summary.c_str(), NotificationIcon::Success)->show();
		}
	}
};

class $modify(MyPauseLayer, PauseLayer) {
	void customSetup() {
		PauseLayer::customSetup();
		activePauseLayer = this;

		auto winSize = CCDirector::sharedDirector()->getWinSize();

		auto recordSpr = CircleButtonSprite::create(CCSprite::createWithSpriteFrameName("GJ_practiceBtn_001.png"), CircleBaseColor::Green, CircleBaseSize::Small);
		auto recordBtn = CCMenuItemSpriteExtra::create(recordSpr, this, menu_selector(MyPauseLayer::onStartRecording));

		auto replaySpr = CircleButtonSprite::create(CCSprite::createWithSpriteFrameName("GJ_playBtn_001.png"), CircleBaseColor::Blue, CircleBaseSize::Small);
		auto replayBtn = CCMenuItemSpriteExtra::create(replaySpr, this, menu_selector(MyPauseLayer::onOpenReplayList));

		auto menu = CCMenu::create();
		menu->setPosition({ 0, 0 });
		menu->addChild(recordBtn);
		menu->addChild(replayBtn);

		recordBtn->setPosition({ winSize.width - 45.f, winSize.height / 2 - 20.f });
		replayBtn->setPosition({ winSize.width - 45.f, winSize.height / 2 - 70.f });

		auto playLayer = PlayLayer::get();
		bool inPractice = playLayer && playLayer->m_isPracticeMode;

		recordBtn->setEnabled(inPractice);
		recordBtn->setOpacity(inPractice ? 255 : 100);

		replayBtn->setEnabled(true);
		replayBtn->setOpacity(255);

		this->addChild(menu, 100);
	}

	void onStartRecording(CCObject*) { RecordNamePopup::create()->show(); }
	void onOpenReplayList(CCObject*) { ReplayListPopup::create(currentLevelID)->show(); }
};