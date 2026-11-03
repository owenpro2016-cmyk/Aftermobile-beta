#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/EffectGameObject.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <cocos2d.h>
#include <cmath>
#include <string>

using namespace geode::prelude;

// =====================================================================================
// 1. INTERFAZ GRÁFICA: HUD Y MENÚ (Igual que antes)
// =====================================================================================

class AssistPopup : public geode::Popup {
protected:
    cocos2d::CCLayer* m_contentLayer = nullptr;
    cocos2d::CCMenu* m_tabMenu = nullptr;
    int m_currentTab = 0;

    void createToggle(cocos2d::CCLayer* parent, std::string id, std::string text, cocos2d::CCPoint pos) {
        auto* label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
        label->setScale(0.35f); label->setAnchorPoint({0.f, 0.5f}); label->setPosition(pos);
        parent->addChild(label);
        auto* toggler = CCMenuItemToggler::create(CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"), CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"), this, menu_selector(AssistPopup::onToggle));
        toggler->setPosition({pos.x + 200.f, pos.y}); toggler->setID(id); toggler->toggle(Mod::get()->getSettingValue<bool>(id));
        auto* menu = CCMenu::create(); menu->setPosition({0, 0}); menu->addChild(toggler); parent->addChild(menu);
    }

    void createIntSetting(cocos2d::CCLayer* parent, std::string id, std::string text, cocos2d::CCPoint pos) {
        auto* label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt");
        label->setScale(0.35f); label->setAnchorPoint({0.f, 0.5f}); label->setPosition(pos); parent->addChild(label);
        int64_t val = Mod::get()->getSettingValue<int64_t>(id);
        auto* valLabel = CCLabelBMFont::create(std::to_string(val).c_str(), "bigFont.fnt");
        valLabel->setScale(0.45f); valLabel->setPosition({pos.x + 140.f, pos.y}); parent->addChild(valLabel);
        auto* menu = CCMenu::create(); menu->setPosition({0, 0}); parent->addChild(menu);
        auto* minus = CCMenuItemSpriteExtra::create(ButtonSprite::create("-", "bigFont.fnt", "GJ_button_04.png", 0.4f), this, menu_selector(AssistPopup::onIntMinus));
        minus->setPosition({pos.x + 110.f, pos.y}); minus->setID(id); menu->addChild(minus);
        auto* plus = CCMenuItemSpriteExtra::create(ButtonSprite::create("+", "bigFont.fnt", "GJ_button_04.png", 0.4f), this, menu_selector(AssistPopup::onIntPlus));
        plus->setPosition({pos.x + 170.f, pos.y}); plus->setID(id); menu->addChild(plus);
    }

    void onToggle(CCObject* sender) { auto* t = typeinfo_cast<CCMenuItemToggler*>(sender); if (t) Mod::get()->setSettingValue<bool>(t->getID(), t->isToggled()); }
    void onIntMinus(CCObject* sender) { auto* b = typeinfo_cast<CCMenuItemSpriteExtra*>(sender); std::string id = b->getID(); int64_t val = Mod::get()->getSettingValue<int64_t>(id) - 1; if (val < 0) val = 0; Mod::get()->setSettingValue<int64_t>(id, val); buildTab(m_currentTab); }
    void onIntPlus(CCObject* sender) { auto* b = typeinfo_cast<CCMenuItemSpriteExtra*>(sender); std::string id = b->getID(); int64_t val = Mod::get()->getSettingValue<int64_t>(id) + 1; Mod::get()->setSettingValue<int64_t>(id, val); buildTab(m_currentTab); }
    void onTab(CCObject* sender) { auto* b = typeinfo_cast<CCMenuItemSpriteExtra*>(sender); if (b) buildTab(b->getTag()); }

    void buildTab(int tab) {
        m_contentLayer->removeAllChildren(); m_currentTab = tab;
        auto* scroll = ScrollLayer::create({260.f, 190.f}); scroll->setPosition({20.f, 20.f}); m_contentLayer->addChild(scroll);
        auto* content = scroll->m_contentLayer; content->setContentSize({260.f, 260.f});
        float y = 240.f;
        if (tab == 0) { 
            createIntSetting(content, "cb-1", "Buffer Max (Ticks)", {10, y}); y -= 35;
            createIntSetting(content, "cb-2", "Tolerancia (Ticks)", {10, y}); y -= 35;
            createToggle(content, "trayectoria-segura", "Evitar Choques Auto", {10, y}); y -= 35;
        } else if (tab == 1) { 
            createToggle(content, "mostrar-trayectoria", "Mostrar Puntos", {10, y}); y -= 35;
            createIntSetting(content, "tr-1", "Longitud Puntos", {10, y}); y -= 35;
        } else if (tab == 2) { 
            createToggle(content, "wv-1", "Wave Recta Auto", {10, y}); y -= 35;
            createToggle(content, "sh-1", "Ship Altitud Auto", {10, y}); y -= 35;
        }
        scroll->moveToTop();
    }

    bool init() override {
        if (!Popup::init(300.f, 280.f)) return false;
        this->setTitle("Aftermobile UI");
        auto* bg = cocos2d::CCLayerColor::create(cocos2d::ccc4(10, 20, 10, 255), 280.f, 240.f); bg->setPosition({10.f, 10.f}); m_mainLayer->addChild(bg, -1);
        m_tabMenu = CCMenu::create(); m_tabMenu->setPosition({0, 0}); m_mainLayer->addChild(m_tabMenu);
        const char* tabs[] = {"Input", "Tray", "Auto"};
        for (int i = 0; i < 3; ++i) {
            auto* btn = CCMenuItemSpriteExtra::create(ButtonSprite::create(tabs[i], "bigFont.fnt", "GJ_button_04.png", 0.4f), this, menu_selector(AssistPopup::onTab));
            btn->setPosition({35.f + i * 85.f, 240.f}); btn->setTag(i); m_tabMenu->addChild(btn);
        }
        m_contentLayer = cocos2d::CCLayer::create(); m_mainLayer->addChild(m_contentLayer); buildTab(0);
        return true;
    }

public:
    static AssistPopup* create() { auto* ret = new AssistPopup(); if (ret->init()) { ret->autorelease(); return ret; } delete ret; return nullptr; }
};

class AssistHUD : public cocos2d::CCLayer {
public:
    cocos2d::CCDrawNode* m_circle = nullptr;
    cocos2d::CCLabelBMFont* m_label = nullptr;
    cocos2d::CCNode* m_sparkles = nullptr;

    static AssistHUD* create() { auto* ret = new AssistHUD(); if (ret->init()) { ret->autorelease(); return ret; } delete ret; return nullptr; }

    bool init() override {
        if (!CCLayer::init()) return false;
        m_circle = cocos2d::CCDrawNode::create();
        m_circle->drawCircle({0, 0}, 28.f, cocos2d::ccc4f(0.1f, 0.1f, 0.1f, 1.f), 3.f, cocos2d::ccc4f(0.f, 1.f, 0.f, 1.f), 32);
        this->addChild(m_circle);
        m_label = cocos2d::CCLabelBMFont::create("am", "bigFont.fnt"); m_label->setScale(0.9f); m_label->setColor({0, 255, 0}); m_label->setPosition({0, -2}); this->addChild(m_label);
        m_sparkles = cocos2d::CCNode::create();
        for(int i=0; i<4; i++) {
            auto* dot = cocos2d::CCDrawNode::create(); dot->drawDot({0, 0}, 2.f, cocos2d::ccc4f(1.f, 1.f, 1.f, 1.f));
            float angle = i * (static_cast<float>(M_PI) / 2.f);
            dot->setPosition({static_cast<float>(std::cos(angle)) * 20.f, static_cast<float>(std::sin(angle)) * 20.f});
            m_sparkles->addChild(dot);
        }
        m_sparkles->runAction(cocos2d::CCRepeatForever::create(cocos2d::CCRotateBy::create(2.0f, 360.f))); this->addChild(m_sparkles);
        this->setTouchEnabled(true); this->setTouchMode(cocos2d::kCCTouchesOneByOne); this->setTouchPriority(-500); 
        this->setPosition({50, cocos2d::CCDirector::get()->getWinSize().height - 100}); this->scheduleUpdate();
        return true;
    }

    void update(float dt) override {
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        bool active = (modo != "desactivado");
        m_label->setColor(active ? cocos2d::ccColor3B(0, 255, 0) : cocos2d::ccColor3B(150, 150, 150));
        m_sparkles->setVisible(active && Mod::get()->getSettingValue<bool>("vis-2"));
        if (Mod::get()->getSettingValue<bool>("vis-1")) {
            if (!m_circle->getActionByTag(123)) {
                auto* pulse = cocos2d::CCRepeatForever::create(cocos2d::CCSequence::create(cocos2d::CCScaleTo::create(0.5f, 1.1f), cocos2d::CCScaleTo::create(0.5f, 1.0f), nullptr));
                pulse->setTag(123); m_circle->runAction(pulse);
            }
        } else { m_circle->stopActionByTag(123); m_circle->setScale(1.0f); }
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override { return this->convertTouchToNodeSpace(touch).getLengthSq() < 900.f; }
    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override { this->setPosition(this->getPosition() + touch->getDelta()); }
    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        auto delta = touch->getDelta();
        if (std::abs(delta.x) < 5 && std::abs(delta.y) < 5) AssistPopup::create()->show();
    }
    void registerWithTouchDispatcher() override { cocos2d::CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true); }
};

// =====================================================================================
// 2. CONFIGURACIÓN Y VARIABLES GLOBALES (NUEVO SISTEMA)
// =====================================================================================

namespace AssistCore {
    constexpr size_t MAX_CACHED_OBSTACLES = 64;
    GameObject* g_cachedObstacles[MAX_CACHED_OBSTACLES];
    size_t g_cachedCount = 0;

    AssistHUD* g_hudInstance = nullptr;

    // Estado de los inputs del jugador
    bool g_isHolding[2] = {false, false}; // Si el usuario tiene el dedo en la pantalla
    bool g_jumpRequested[2] = {false, false}; // Si el usuario quiere saltar pero el mod lo está reteniendo
    uint64_t g_requestTick[2] = {0, 0};

    // Variables del jugador para el tick actual
    PlayerObject* g_p1 = nullptr;
    PlayerObject* g_p2 = nullptr;
}

// =====================================================================================
// 3. SIMULADOR DE FÍSICA Y PREDICCIÓN
// =====================================================================================

namespace PhysicsEngine {
    constexpr float GRAVITY = 0.958199f;
    constexpr float JUMP_VELOCITY = 11.180032f;
    constexpr float PLAYER_SIZE = 30.0f;

    void updateObstacleCache(PlayLayer* pl, PlayerObject* p1) {
        AssistCore::g_cachedCount = 0;
        if (!pl || !p1 || !pl->m_objects) return;

        float pX = p1->getPositionX();
        float pY = p1->getPositionY();

        for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (AssistCore::g_cachedCount >= AssistCore::MAX_CACHED_OBSTACLES) break;
            if (!obj) continue;
            
            auto type = obj->m_objectType;
            if (type == GameObjectType::Solid || type == GameObjectType::Hazard) {
                float oX = obj->getPositionX();
                float oY = obj->getPositionY();
                if (oX > pX - 100.f && oX < pX + 400.f && oY > pY - 200.f && oY < pY + 200.f) {
                    AssistCore::g_cachedObstacles[AssistCore::g_cachedCount++] = obj;
                }
            }
        }
    }

    bool checkCollision(float x, float y) {
        for (size_t i = 0; i < AssistCore::g_cachedCount; i++) {
            auto* obj = AssistCore::g_cachedObstacles[i];
            auto rect = obj->getObjectRect();
            if (x + (PLAYER_SIZE/2.f) > rect.getMinX() && x - (PLAYER_SIZE/2.f) < rect.getMaxX() &&
                y + (PLAYER_SIZE/2.f) > rect.getMinY() && y - (PLAYER_SIZE/2.f) < rect.getMaxY()) {
                return true;
            }
        }
        return false;
    }

    bool isJumpSafe(PlayerObject* p1) {
        if (!p1) return false;
        float pX = p1->getPositionX();
        float pY = p1->getPositionY();
        float vX = 8.4f * p1->m_speedMultiplier; 
        float vY = JUMP_VELOCITY; 

        for (int t = 1; t <= 30; ++t) {
            float nextX = pX + (vX * t);
            float nextY = pY + (vY * t) - (0.5f * GRAVITY * t * t);
            if (checkCollision(nextX, nextY)) return false;
            if (nextY < -50.f) break;
        }
        return true;
    }
}

// =====================================================================================
// 4. HOOKS DEL JUEGO (LÓGICA PRINCIPAL)
// =====================================================================================

class $modify(InputAssistBase, GJBaseGameLayer) {
    void update(float dt) {
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        if (modo != "desactivado") {
            auto* pl = PlayLayer::get();
            if (pl && pl->m_player1) {
                PhysicsEngine::updateObstacleCache(pl, pl->m_player1);

                for (int i = 0; i < 2; i++) {
                    bool isP1 = (i == 0);
                    PlayerObject* player = isP1 ? pl->m_player1 : pl->m_player2;
                    if (!player) continue;

                    // Si el usuario quiere saltar pero lo estamos reteniendo
                    if (AssistCore::g_jumpRequested[i]) {
                        bool autoSafe = Mod::get()->getSettingValue<bool>("trayectoria-segura");
                        bool safe = !autoSafe || PhysicsEngine::isJumpSafe(player);

                        // Si es seguro o ya pasó el tiempo límite (buffer), ¡ejecutamos el salto directamente en el jugador!
                        int64_t maxBuffer = Mod::get()->getSettingValue<int64_t>("cb-1");
                        bool timeout = (Mod::get()->getSettingValue<int64_t>("cb-1") == 0) || ((pl->m_gameState.m_currentProgress - AssistCore::g_requestTick[i]) > maxBuffer);

                        if (safe || timeout) {
                            // Llamada directa al jugador (SÚPER EFECTIVA)
                            if (player->m_isOnGround || player->m_isShip || player->m_isBird || player->m_isDart) {
                                player->pushButton(PlayerButton::Jump);
                            }
                            AssistCore::g_jumpRequested[i] = false;
                        }
                    } 
                    
                    // Si el mod retuvo el salto, pero el usuario ya soltó la pantalla y el salto era inseguro
                    if (!AssistCore::g_isHolding[i] && AssistCore::g_jumpRequested[i]) {
                         // Cancelamos el salto si ya no está tocando la pantalla
                         AssistCore::g_jumpRequested[i] = false;
                    }
                }
            }
        }
        GJBaseGameLayer::update(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        std::string modo = Mod::get()->getSettingValue<std::string>("modo-asistencia");
        
        // Si el mod está apagado, pasamos el input normal
        if (modo == "desactivado") {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        int p = isPlayer1 ? 0 : 1;
        auto* pl = PlayLayer::get();
        if (!pl) return;

        PlayerObject* player = isPlayer1 ? pl->m_player1 : pl->m_player2;
        if (!player) return;

        if (down) {
            AssistCore::g_isHolding[p] = true;
            
            // Si está en el suelo, comprobamos si es seguro. Si lo es, le damos el salto directo.
            // Si no lo es, lo retenemos (g_jumpRequested = true) para que el update lo salte cuando toque.
            bool autoSafe = Mod::get()->getSettingValue<bool>("trayectoria-segura");
            
            if (player->m_isOnGround && autoSafe) {
                if (PhysicsEngine::isJumpSafe(player)) {
                    player->pushButton(PlayerButton::Jump);
                } else {
                    AssistCore::g_jumpRequested[p] = true;
                    AssistCore::g_requestTick[p] = pl->m_gameState.m_currentProgress;
                }
            } else {
                // Si está en el aire (orbes) o la auto-seguridad está apagada, salta directo
                player->pushButton(PlayerButton::Jump);
            }
        } else {
            AssistCore::g_isHolding[p] = false;
            AssistCore::g_jumpRequested[p] = false;
            // Al soltar, llamamos al release directo
            player->releaseButton(PlayerButton::Jump);
        }
    }
};

class $modify(InputAssistPlay, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        
        AssistCore::g_isHolding[0] = AssistCore::g_isHolding[1] = false;
        AssistCore::g_jumpRequested[0] = AssistCore::g_jumpRequested[1] = false;
        
        if (!AssistCore::g_hudInstance) {
            AssistCore::g_hudInstance = AssistHUD::create();
            this->addChild(AssistCore::g_hudInstance, 9999);
        }
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        AssistCore::g_isHolding[0] = AssistCore::g_isHolding[1] = false;
        AssistCore::g_jumpRequested[0] = AssistCore::g_jumpRequested[1] = false;
    }

    void update(float dt) {
        if (Mod::get()->getSettingValue<bool>("mostrar-trayectoria") && m_player1) {
            auto* drawNode = typeinfo_cast<cocos2d::CCDrawNode*>(this->getChildByID("trajectory-draw-node"));
            if (!drawNode) {
                drawNode = cocos2d::CCDrawNode::create();
                drawNode->setID("trajectory-draw-node");
                this->addChild(drawNode, 100);
            }
            drawNode->clear();
            
            float x = m_player1->getPositionX();
            float y = m_player1->getPositionY();
            float vx = 8.4f * m_player1->m_speedMultiplier;
            float vy = m_player1->m_yVelocity;
            
            int64_t maxTicks = Mod::get()->getSettingValue<int64_t>("tr-1");
            
            for (int i = 0; i < maxTicks; ++i) {
                x += vx;
                if (m_player1->m_isShip) vy += 0.4f;
                else vy -= 0.958f; 
                y += vy;
                drawNode->drawDot({x, y}, 2.5f, cocos2d::ccc4f(1.0f, 0.2f, 0.2f, 0.5f));
                if (y < 0) break;
            }
        }
        PlayLayer::update(dt);
    }
};

class $modify(PauseLayerHook, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto* menu = getChildByID("right-button-menu");
        if (!menu) return;
        auto* btn = CCMenuItemSpriteExtra::create(CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png"), this, menu_selector(PauseLayerHook::abrirPanel));
        menu->addChild(btn);
    }
    void abrirPanel(CCObject*) { AssistPopup::create()->show(); }
};
