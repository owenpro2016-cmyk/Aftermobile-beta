#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/ui/Popup.hpp>
#include <cocos2d.h>
#include <cmath>
#include <deque>
#include <map>

using namespace geode::prelude;

// =====================================================================================
// 1. CEREBRO Y FÍSICA (AfterEngine)
// =====================================================================================

namespace AfterEngine {
    constexpr float GRAVITY = 0.958199f;
    constexpr float JUMP_VELOCITY = 11.180032f;
    constexpr float PLAYER_SIZE = 30.0f;
    constexpr size_t MAX_HISTORY = 10;

    struct Hitbox { float minX, minY, maxX, maxY; bool isHazard; bool isOrb; };
    struct Snapshot { float x, y, vY; bool onGround; uint64_t tick; };

    std::array<Hitbox, 200> g_hitboxes;
    size_t g_hbCount = 0;
    std::deque<Snapshot> g_history;
    std::map<int, int> g_levelMemory;

    uint64_t g_currentTick = 0;
    bool g_holding[2] = {false, false};
    bool g_bufferedJump[2] = {false, false};
    uint64_t g_jumpStartTick[2] = {0, 0};
    int g_lastDelayUsed[2] = {0, 0};

    int getMaxTicks(PlayerObject* p) {
        std::string mode = Mod::get()->getSettingValue<std::string>("assist-mode");
        if (mode == "Desactivado") return 0;
        if (mode == "Personalizado") {
            if (p->m_isDart) return Mod::get()->getSettingValue<int64_t>("ticks-wave");
            if (p->m_isShip) return Mod::get()->getSettingValue<int64_t>("ticks-ship");
            return Mod::get()->getSettingValue<int64_t>("ticks-cube");
        }
        bool isWave = p->m_isDart;
        if (mode == "Suave") return isWave ? 2 : 6;
        if (mode == "Balanceado") return isWave ? 4 : 12;
        return isWave ? 8 : 24; 
    }

    void updateHitboxes(PlayLayer* pl, PlayerObject* p) {
        g_hbCount = 0;
        if (!pl || !p || !pl->m_objects) return;
        float px = (float)p->getPositionX(), py = (float)p->getPositionY();
        
        for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (g_hbCount >= g_hitboxes.size()) break;
            if (!obj) continue;
            auto t = obj->m_objectType;
            
            if (t == GameObjectType::Solid || t == GameObjectType::Hazard || t == GameObjectType::Special) {
                float ox = (float)obj->getPositionX(), oy = (float)obj->getPositionY();
                if (ox > px - 150.f && ox < px + 600.f && oy > py - 300.f && oy < py + 300.f) {
                    auto r = obj->getObjectRect();
                    bool isOrb = (t == GameObjectType::Special && obj->m_objectID >= 17 && obj->m_objectID <= 24);
                    g_hitboxes[g_hbCount++] = { r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY(), t == GameObjectType::Hazard, isOrb };
                }
            }
        }
    }

    bool isSafe(float startX, float startY, PlayerObject* p) {
        float vX = 8.4f * (p->m_speedMultiplier > 0 ? p->m_speedMultiplier : 1.0f);
        float vY = JUMP_VELOCITY;
        bool smartOrbs = Mod::get()->getSettingValue<bool>("smart-orbs");
        
        for (int t = 1; t <= 30; ++t) {
            float nx = startX + (vX * t);
            float ny = startY + (vY * t) - (0.5f * GRAVITY * t * t);
            
            for (size_t i = 0; i < g_hbCount; i++) {
                auto& hb = g_hitboxes[i];
                
                // Si es orbe, simula el salto y sigue
                if (hb.isOrb && smartOrbs) {
                    if (nx + PLAYER_SIZE/2 > hb.minX && nx - PLAYER_SIZE/2 < hb.maxX && 
                        ny + PLAYER_SIZE/2 > hb.minY && ny - PLAYER_SIZE/2 < hb.maxY) {
                        vY = JUMP_VELOCITY; 
                    }
                    continue;
                }
                
                // Colision
                if (nx + PLAYER_SIZE/2 - 2 > hb.minX && nx - PLAYER_SIZE/2 + 2 < hb.maxX && 
                    ny + PLAYER_SIZE/2 - 2 > hb.minY && ny - PLAYER_SIZE/2 + 2 < hb.maxY) {
                    return false;
                }
            }
            if (ny <= startY && t > 3) break;
        }
        return true;
    }

    void tryRewind(PlayerObject* p) {
        if (!Mod::get()->getSettingValue<bool>("caf-enabled") || g_history.empty()) return;
        int maxRw = std::min((int)g_history.size(), (int)Mod::get()->getSettingValue<int64_t>("caf-max-rewind"));
        
        for (int i = 1; i <= maxRw; i++) {
            auto past = g_history[g_history.size() - i];
            if (past.onGround && isSafe(past.x, past.y, p)) {
                p->setPositionX(past.x); p->setPositionY(past.y);
                p->m_yVelocity = JUMP_VELOCITY; p->m_isOnGround = false;
                return;
            }
        }
    }
}

// =====================================================================================
// 2. UI: BOLITA CON TEXTO DE ESTADO
// =====================================================================================

class AeroHUD : public cocos2d::CCLayer {
public:
    static AeroHUD* create() {
        auto* ret = new AeroHUD(); 
        if (ret->init()) { ret->autorelease(); return ret; } 
        delete ret; return nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;
        auto col = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-primary-color");
        
        auto* circle = cocos2d::CCDrawNode::create();
        circle->drawCircle({0, 0}, 22.f, cocos2d::ccc4f(0.1f, 0.1f, 0.1f, 0.9f), 2.f, cocos2d::ccc4f(col.r/255.f, col.g/255.f, col.b/255.f, 1.f), 32);
        this->addChild(circle);
        
        auto* label = cocos2d::CCLabelBMFont::create("am", "bigFont.fnt");
        label->setScale(0.7f); label->setColor(col); label->setPosition({0, 0});
        this->addChild(label);

        if (Mod::get()->getSettingValue<bool>("debug-mode")) {
            auto* debug = cocos2d::CCLabelBMFont::create("Listo", "chatFont.fnt");
            debug->setScale(0.7f);
            debug->setPosition({35, -5});
            debug->setAnchorPoint({0.f, 0.5f});
            debug->setID("debug-label");
            this->addChild(debug);
        }
        
        this->setTouchEnabled(true); this->setTouchMode(cocos2d::kCCTouchesOneByOne); 
        this->setTouchPriority(-500);
        
        float hx = (float)Mod::get()->getSettingValue<double>("hud-x");
        float hy = (float)Mod::get()->getSettingValue<double>("hud-y");
        this->setPosition({hx, cocos2d::CCDirector::get()->getWinSize().height - hy});
        this->scheduleUpdate();
        return true;
    }

    void update(float dt) override {
        auto* lbl = typeinfo_cast<CCLabelBMFont*>(this->getChildByID("debug-label"));
        if (!lbl) return;

        if (AfterEngine::g_bufferedJump[0]) {
            lbl->setString(fmt::format("Reteniendo: {}tk", AfterEngine::g_currentTick - AfterEngine::g_jumpStartTick[0]).c_str());
            lbl->setColor({255, 255, 0}); // Amarillo
        } else if (AfterEngine::g_holding[0]) {
            lbl->setString("Manteniendo");
            lbl->setColor({0, 255, 0}); // Verde
        } else {
            lbl->setString(fmt::format("Listo (Delay: {})", AfterEngine::g_lastDelayUsed[0]).c_str());
            lbl->setColor({200, 200, 200}); // Gris
        }
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override { return this->convertTouchToNodeSpace(touch).getLengthSq() < 400.f; }
    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override { this->setPosition(this->getPosition() + touch->getDelta()); }
    void registerWithTouchDispatcher() override { cocos2d::CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true); }
};

// =====================================================================================
// 3. HOOKS DEL JUEGO
// =====================================================================================

class $modify(PlayerHook, PlayerObject) {
    void pushButton(PlayerButton b) {
        if (b != PlayerButton::Jump) { PlayerObject::pushButton(b); return; }
        auto* pl = PlayLayer::get(); if (!pl) return;
        int p = (this == pl->m_player1) ? 0 : 1;
        
        AfterEngine::g_holding[p] = true;
        
        if (this->m_isOnGround) {
            if (Mod::get()->getSettingValue<std::string>("assist-mode") == "Desactivado") {
                // CAF se ejecuta al instante si esta desactivado el buffer
                AfterEngine::tryRewind(this);
                PlayerObject::pushButton(b); 
            } else {
                // RETENEMOS EL CLICK
                AfterEngine::g_bufferedJump[p] = true;
                AfterEngine::g_jumpStartTick[p] = AfterEngine::g_currentTick;
                // NO llamamos al original, evitamos que salte ya
            }
        } else {
            PlayerObject::pushButton(b);
        }
    }

    void releaseButton(PlayerButton b) {
        if (b != PlayerButton::Jump) { PlayerObject::releaseButton(b); return; }
        auto* pl = PlayLayer::get(); if (!pl) return;
        int p = (this == pl->m_player1) ? 0 : 1;
        
        AfterEngine::g_holding[p] = false;
        if (AfterEngine::g_bufferedJump[p]) { 
            AfterEngine::g_bufferedJump[p] = false; 
            return; // Cancelamos el salto retenido
        }
        PlayerObject::releaseButton(b);
    }
};

class $modify(BaseHook, GJBaseGameLayer) {
    void update(float dt) {
        AfterEngine::g_currentTick++;
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1) { GJBaseGameLayer::update(dt); return; }

        // Historial para CAF
        AfterEngine::g_history.push_back({ 
            (float)pl->m_player1->getPositionX(), (float)pl->m_player1->getPositionY(), 
            (float)pl->m_player1->m_yVelocity, pl->m_player1->m_isOnGround, AfterEngine::g_currentTick 
        });
        if (AfterEngine::g_history.size() > AfterEngine::MAX_HISTORY) AfterEngine::g_history.pop_front();

        if (Mod::get()->getSettingValue<std::string>("assist-mode") != "Desactivado") {
            AfterEngine::updateHitboxes(pl, pl->m_player1);
            
            for (int i = 0; i < 2; i++) {
                auto* p = (i == 0) ? pl->m_player1 : pl->m_player2;
                if (!p || !AfterEngine::g_bufferedJump[i]) continue;
                
                int maxT = AfterEngine::getMaxTicks(p);
                uint64_t elapsed = AfterEngine::g_currentTick - AfterEngine::g_jumpStartTick[i];
                
                if (p->m_isOnGround) {
                    // Si es seguro o llego al limite, INYECTAMOS EL SALTO
                    if (AfterEngine::isSafe((float)p->getPositionX(), (float)p->getPositionY(), p) || elapsed >= maxT) {
                        
                        // Esto hace que el juego procese el salto de verdad
                        GJBaseGameLayer::handleButton(true, 1, i == 0);
                        
                        AfterEngine::g_lastDelayUsed[i] = elapsed;
                        AfterEngine::g_bufferedJump[i] = false;
                        
                        if (!AfterEngine::g_holding[i]) {
                            GJBaseGameLayer::handleButton(false, 1, i == 0);
                        }
                    }
                } else {
                    AfterEngine::g_bufferedJump[i] = false;
                }
            }
        }
        GJBaseGameLayer::update(dt);
    }
};

// =====================================================================================
// 4. HOOKS DE UI Y DIBUJO
// =====================================================================================

class $modify(MenuHook, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (Mod::get()->getSettingValue<bool>("show-hud")) this->addChild(AeroHUD::create(), 9999);
        return true;
    }
};

class $modify(PauseHook, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        if (auto* menu = getChildByID("right-button-menu")) {
            menu->addChild(CCMenuItemSpriteExtra::create(CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png"), this, menu_selector(PauseHook::openUI)));
        }
    }
    void openUI(CCObject*) { FLAlertLayer::create("Aftermobile", "Configuracion en el menu de Geode.", "OK")->show(); }
};

class $modify(PlayHook, PlayLayer) {
    bool init(GJGameLevel* l, bool r, bool d) {
        if (!PlayLayer::init(l, r, d)) return false;
        AfterEngine::g_history.clear();
        if (Mod::get()->getSettingValue<bool>("show-hud")) this->addChild(AeroHUD::create(), 9999);
        return true;
    }

    void draw() {
        PlayLayer::draw();
        if (!m_player1) return;
        
        auto* drawNode = typeinfo_cast<cocos2d::CCDrawNode*>(this->getChildByID("draw-node"));
        if (!drawNode) { 
            drawNode = cocos2d::CCDrawNode::create(); 
            drawNode->setID("draw-node"); 
            this->addChild(drawNode, 100); 
        }
        drawNode->clear();

        if (Mod::get()->getSettingValue<bool>("show-traj")) {
            float px = (float)m_player1->getPositionX(), py = (float)m_player1->getPositionY();
            float vx = 8.4f * m_player1->m_speedMultiplier, vy = AfterEngine::JUMP_VELOCITY;
            bool safe = AfterEngine::isSafe(px, py, m_player1);
            
            auto col = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-primary-color");
            cocos2d::ccColor4F c = safe ? 
                cocos2d::ccc4f(col.r/255.f, col.g/255.f, col.b/255.f, 0.6f) : 
                cocos2d::ccc4f(1, 0, 0, 0.6f); 

            int len = (int)Mod::get()->getSettingValue<int64_t>("traj-length");
            for (int i = 1; i <= len; ++i) {
                drawNode->drawDot({px + (vx * i), py + (vy * i) - (0.5f * AfterEngine::GRAVITY * i * i)}, 2.5f, c);
            }
        }
    }
};
