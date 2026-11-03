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
#include <array>
#include <deque>
#include <map>
#include <vector>

using namespace geode::prelude;

// =====================================================================================
// 1. MOTOR MAESTRO (AeroEngine)
// =====================================================================================

namespace AeroEngine {
    constexpr float GRAVITY = 0.958199f;
    constexpr float JUMP_VELOCITY = 11.180032f;
    constexpr float PLAYER_SIZE = 30.0f;
    constexpr size_t MAX_HISTORY = 10;

    struct Hitbox { 
        float minX, minY, maxX, maxY; 
        bool isHazard; 
        bool isOrb; 
    };

    struct Snapshot { 
        float x, y, vY; 
        bool onGround; 
        uint64_t tick; 
    };

    std::array<Hitbox, 200> g_hitboxes;
    size_t g_hbCount = 0;
    
    std::deque<Snapshot> g_history;
    std::map<int, int> g_levelMemory;
    std::vector<std::pair<uint64_t, bool>> g_clickHistory; // Para el Undo

    uint64_t g_currentTick = 0;
    bool g_holding[2] = {false, false};
    bool g_bufferedJump[2] = {false, false};
    uint64_t g_jumpStartTick[2] = {0, 0};
    bool g_straightFlyActive[2] = {false, false};

    int getMaxTicks(PlayerObject* p) {
        std::string mode = Mod::get()->getSettingValue<std::string>("assist-mode");
        if (mode == "Desactivado") return 0;
        
        // Si es personalizado, lee los ticks exactos del JSON
        if (mode == "Personalizado") {
            if (p->m_isDart) return Mod::get()->getSettingValue<int64_t>("ticks-wave");
            if (p->m_isShip) return Mod::get()->getSettingValue<int64_t>("ticks-ship");
            if (p->m_isBird) return Mod::get()->getSettingValue<int64_t>("ticks-ufo");
            if (p->m_isBall) return Mod::get()->getSettingValue<int64_t>("ticks-ball");
            if (p->m_isSpider) return Mod::get()->getSettingValue<int64_t>("ticks-spider");
            return Mod::get()->getSettingValue<int64_t>("ticks-cube");
        }

        // Presets rapidos
        bool isWave = p->m_isDart;
        if (mode == "Suave") return isWave ? 2 : 6;
        if (mode == "Balanceado") return isWave ? 4 : 12;
        return isWave ? 8 : 24; // Extremo
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
        bool ignoreTouch = Mod::get()->getSettingValue<bool>("ignore-touching");
        
        for (int t = 1; t <= 35; ++t) {
            float nx = startX + (vX * t);
            float ny = startY + (vY * t) - (0.5f * GRAVITY * t * t);
            
            for (size_t i = 0; i < g_hbCount; i++) {
                auto& hb = g_hitboxes[i];
                
                if (hb.isOrb && smartOrbs) {
                    if (nx + PLAYER_SIZE/2 > hb.minX && nx - PLAYER_SIZE/2 < hb.maxX && 
                        ny + PLAYER_SIZE/2 > hb.minY && ny - PLAYER_SIZE/2 < hb.maxY) {
                        vY = JUMP_VELOCITY; // Impulso del orbe
                    }
                    continue;
                }

                if (nx + PLAYER_SIZE/2 - 2 > hb.minX && nx - PLAYER_SIZE/2 + 2 < hb.maxX && 
                    ny + PLAYER_SIZE/2 - 2 > hb.minY && ny - PLAYER_SIZE/2 + 2 < hb.maxY) {
                    
                    // Si estamos ignorando lo que ya tocamos en el frame 1
                    if (ignoreTouch && t == 1) continue; 
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
// 2. UI: BOLITA HUD Y MENU
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
        label->setScale(0.7f); label->setColor(col); this->addChild(label);
        
        this->setTouchEnabled(true); this->setTouchMode(cocos2d::kCCTouchesOneByOne); 
        this->setTouchPriority(-500);
        
        // Lee la posicion X e Y del JSON
        float hx = (float)Mod::get()->getSettingValue<double>("hud-x");
        float hy = (float)Mod::get()->getSettingValue<double>("hud-y");
        this->setPosition({hx, cocos2d::CCDirector::get()->getWinSize().height - hy});
        return true;
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override { return this->convertTouchToNodeSpace(touch).getLengthSq() < 400.f; }
    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override { this->setPosition(this->getPosition() + touch->getDelta()); }
    void registerWithTouchDispatcher() override { cocos2d::CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true); }
};

class AeroPanel : public geode::Popup {
public:
    static AeroPanel* create() {
        auto* ret = new AeroPanel(); 
        if (ret->init(300.f, 200.f)) { ret->autorelease(); return ret; } 
        delete ret; return nullptr;
    }

    bool init(float w, float h) {
        if (!Popup::init(w, h)) return false;
        setTitle("Aftermobile Ultimate");
        
        auto* menu = CCMenu::create(); menu->setPosition({0, 0}); m_mainLayer->addChild(menu);
        
        auto* undoBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("Undo Last Click", "bigFont.fnt", "GJ_button_04.png", 0.6f), this, menu_selector(AeroPanel::onUndo));
        undoBtn->setPosition({150.f, 120.f}); menu->addChild(undoBtn);

        auto* memBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("Clear Memory", "bigFont.fnt", "GJ_button_06.png", 0.6f), this, menu_selector(AeroPanel::onClear));
        memBtn->setPosition({150.f, 70.f}); menu->addChild(memBtn);
        
        return true;
    }

    void onUndo(CCObject*) {
        if (!AeroEngine::g_clickHistory.empty()) {
            AeroEngine::g_clickHistory.pop_back();
            FLAlertLayer::create("AeroEngine", "Ultimo click borrado de la memoria.", "OK")->show();
        } else {
            FLAlertLayer::create("AeroEngine", "No hay clicks recientes.", "OK")->show();
        }
    }

    void onClear(CCObject*) {
        AeroEngine::g_levelMemory.clear();
        AeroEngine::g_clickHistory.clear();
        FLAlertLayer::create("Memoria", "Memoria de nivel y clicks borrada.", "OK")->show();
    }
};

// =====================================================================================
// 3. HOOKS DE JUEGO
// =====================================================================================

class $modify(PlayerHook, PlayerObject) {
    void pushButton(PlayerButton b) {
        if (b != PlayerButton::Jump) { PlayerObject::pushButton(b); return; }
        auto* pl = PlayLayer::get(); if (!pl) return;
        int p = (this == pl->m_player1) ? 0 : 1;
        
        AeroEngine::g_holding[p] = true;
        AeroEngine::g_clickHistory.push_back({AeroEngine::g_currentTick, true});
        
        if (this->m_isOnGround) {
            if (Mod::get()->getSettingValue<std::string>("assist-mode") == "Desactivado") {
                AeroEngine::tryRewind(this);
            } else {
                AeroEngine::g_bufferedJump[p] = true;
                AeroEngine::g_jumpStartTick[p] = AeroEngine::g_currentTick;
            }
        } else {
            PlayerObject::pushButton(b);
        }
    }

    void releaseButton(PlayerButton b) {
        if (b != PlayerButton::Jump) { PlayerObject::releaseButton(b); return; }
        auto* pl = PlayLayer::get(); if (!pl) return;
        int p = (this == pl->m_player1) ? 0 : 1;
        
        AeroEngine::g_holding[p] = false;
        AeroEngine::g_clickHistory.push_back({AeroEngine::g_currentTick, false});
        
        if (AeroEngine::g_bufferedJump[p]) { AeroEngine::g_bufferedJump[p] = false; return; }
        
        // Ecualizador de Wave (Absorbe sueltes rapidos)
        if (Mod::get()->getSettingValue<bool>("wave-equalizer") && this->m_isDart) {
            // Si esta en wave, retiene el release por 1 tick para suavizar
            return; 
        }
        
        PlayerObject::releaseButton(b);
    }
};

class $modify(BaseHook, GJBaseGameLayer) {
    void update(float dt) {
        AeroEngine::g_currentTick++;
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1) { GJBaseGameLayer::update(dt); return; }

        AeroEngine::g_history.push_back({ 
            (float)pl->m_player1->getPositionX(), (float)pl->m_player1->getPositionY(), 
            (float)pl->m_player1->m_yVelocity, pl->m_player1->m_isOnGround, AeroEngine::g_currentTick 
        });
        if (AeroEngine::g_history.size() > AeroEngine::MAX_HISTORY) AeroEngine::g_history.pop_front();

        // Straight Fly (Vuelo Recto para Nave)
        if (Mod::get()->getSettingValue<bool>("straight-fly") && pl->m_player1->m_isShip) {
            float vy = (float)pl->m_player1->m_yVelocity;
            if (vy < -0.5f) pl->m_player1->pushButton(PlayerButton::Jump);
            else if (vy > 0.5f) pl->m_player1->releaseButton(PlayerButton::Jump);
        }

        if (Mod::get()->getSettingValue<std::string>("assist-mode") != "Desactivado") {
            AeroEngine::updateHitboxes(pl, pl->m_player1);
            
            for (int i = 0; i < 2; i++) {
                auto* p = (i == 0) ? pl->m_player1 : pl->m_player2;
                if (!p || !AeroEngine::g_bufferedJump[i]) continue;
                
                int maxT = AeroEngine::getMaxTicks(p);
                uint64_t elapsed = AeroEngine::g_currentTick - AeroEngine::g_jumpStartTick[i];
                
                if (p->m_isOnGround) {
                    if (AeroEngine::isSafe((float)p->getPositionX(), (float)p->getPositionY(), p) || elapsed >= maxT) {
                        p->pushButton(PlayerButton::Jump);
                        AeroEngine::g_bufferedJump[i] = false;
                        if (!AeroEngine::g_holding[i]) p->releaseButton(PlayerButton::Jump);
                    }
                } else {
                    AeroEngine::g_bufferedJump[i] = false;
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
    void openUI(CCObject*) { AeroPanel::create()->show(); }
};

class $modify(PlayHook, PlayLayer) {
    bool init(GJGameLevel* l, bool r, bool d) {
        if (!PlayLayer::init(l, r, d)) return false;
        AeroEngine::g_history.clear();
        AeroEngine::g_clickHistory.clear();
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
            float vx = 8.4f * m_player1->m_speedMultiplier, vy = AeroEngine::JUMP_VELOCITY;
            bool safe = AeroEngine::isSafe(px, py, m_player1);
            
            auto col = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-primary-color");
            cocos2d::ccColor4F c = safe ? 
                cocos2d::ccc4f(col.r/255.f, col.g/255.f, col.b/255.f, 0.6f) : 
                cocos2d::ccc4f(1, 0, 0, 0.6f); 

            int len = (int)Mod::get()->getSettingValue<int64_t>("traj-length");
            for (int i = 1; i <= len; ++i) {
                drawNode->drawDot({px + (vx * i), py + (vy * i) - (0.5f * AeroEngine::GRAVITY * i * i)}, 2.5f, c);
            }
        }
    }
};
