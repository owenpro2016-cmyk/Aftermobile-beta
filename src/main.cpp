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

using namespace geode::prelude;

// =====================================================================================
// 1. MOTOR FÍSICO Y LÓGICA (AfterEngine)
// =====================================================================================

namespace AfterEngine {
    constexpr float GRAVITY = 0.958199f;
    constexpr float JUMP_VELOCITY = 11.180032f;
    constexpr float PLAYER_SIZE = 30.0f;
    constexpr size_t MAX_HISTORY = 10;

    struct Hitbox { 
        float minX, minY, maxX, maxY; 
        bool isHazard; 
    };

    struct Snapshot { 
        float x, y, vY; 
        bool onGround; 
        uint64_t tick; 
    };

    std::array<Hitbox, 150> g_hitboxes;
    size_t g_hbCount = 0;
    
    std::deque<Snapshot> g_history;
    std::map<uint64_t, int> g_levelMemory; 
    
    uint64_t g_currentTick = 0;
    bool g_holding[2] = {false, false};
    bool g_bufferedJump[2] = {false, false};
    uint64_t g_jumpStartTick[2] = {0, 0};

    int getMaxTicks(PlayerObject* p) {
        std::string mode = Mod::get()->getSettingValue<std::string>("assist-mode");
        if (mode == "Desactivado") return 0;
        bool isWave = p->m_isDart;
        if (mode == "Suave") return isWave ? 2 : 6;
        if (mode == "Balanceado") return isWave ? 4 : 12;
        return isWave ? 8 : 24; 
    }

    void updateHitboxes(PlayLayer* pl, PlayerObject* p) {
        g_hbCount = 0;
        if (!pl || !p || !pl->m_objects) return;
        float px = p->getPositionX(), py = p->getPositionY();
        
        for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (g_hbCount >= g_hitboxes.size()) break;
            if (!obj) continue;
            auto t = obj->m_objectType;
            
            if (t == GameObjectType::Solid || t == GameObjectType::Hazard) {
                float ox = obj->getPositionX(), oy = obj->getPositionY();
                if (ox > px - 150.f && ox < px + 500.f && oy > py - 300.f && oy < py + 300.f) {
                    auto r = obj->getObjectRect();
                    g_hitboxes[g_hbCount++] = { r.getMinX(), r.getMinY(), r.getMaxX(), r.getMaxY(), t == GameObjectType::Hazard };
                }
            }
        }
    }

    bool isSafe(float startX, float startY, PlayerObject* p) {
        float vX = 8.4f * (p->m_speedMultiplier > 0 ? p->m_speedMultiplier : 1.0f);
        float vY = JUMP_VELOCITY;
        
        for (int t = 1; t <= 25; ++t) {
            float nx = startX + (vX * t);
            float ny = startY + (vY * t) - (0.5f * GRAVITY * t * t);
            
            for (size_t i = 0; i < g_hbCount; i++) {
                auto& hb = g_hitboxes[i];
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
        int maxRw = std::min((int)g_history.size(), 5);
        
        for (int i = 1; i <= maxRw; i++) {
            auto past = g_history[g_history.size() - i];
            if (past.onGround && isSafe(past.x, past.y, p)) {
                p->setPositionX(past.x); 
                p->setPositionY(past.y);
                p->m_yVelocity = JUMP_VELOCITY; 
                p->m_isOnGround = false;
                return;
            }
        }
    }
}

// =====================================================================================
// 2. UI: BOLITA HUD Y MENU DE CONTROL
// =====================================================================================

class AfterHUD : public cocos2d::CCLayer {
public:
    static AfterHUD* create() {
        auto* ret = new AfterHUD(); 
        if (ret->init()) { ret->autorelease(); return ret; } 
        delete ret; return nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;
        auto col = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-primary-color");
        
        auto* circle = cocos2d::CCDrawNode::create();
        // drawCircle usa ccc4f, asi que esta bien
        circle->drawCircle({0, 0}, 22.f, cocos2d::ccc4f(0.1f, 0.1f, 0.1f, 0.9f), 2.f, cocos2d::ccc4f(col.r/255.f, col.g/255.f, col.b/255.f, 1.f), 32);
        this->addChild(circle);
        
        auto* label = cocos2d::CCLabelBMFont::create("am", "bigFont.fnt");
        label->setScale(0.7f); 
        label->setColor(col); 
        this->addChild(label);
        
        this->setTouchEnabled(true); 
        this->setTouchMode(cocos2d::kCCTouchesOneByOne); 
        this->setTouchPriority(-500);
        this->setPosition({50, cocos2d::CCDirector::get()->getWinSize().height - 100});
        return true;
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override { 
        return this->convertTouchToNodeSpace(touch).getLengthSq() < 400.f; 
    }
    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override { 
        this->setPosition(this->getPosition() + touch->getDelta()); 
    }
    void registerWithTouchDispatcher() override { 
        cocos2d::CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true); 
    }
};

class AfterPanel : public geode::Popup {
public: // IMPORTANTE: public para que funcionen los botones
    static AfterPanel* create() {
        auto* ret = new AfterPanel(); 
        if (ret->init(280.f, 200.f)) { ret->autorelease(); return ret; } 
        delete ret; return nullptr;
    }

    bool init(float w, float h) {
        if (!Popup::init(w, h)) return false; // Inicializa m_mainLayer
        setTitle("Aftermobile Control");
        
        auto* menu = CCMenu::create(); 
        menu->setPosition({0, 0}); 
        m_mainLayer->addChild(menu);
        
        auto* modeBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("Cambiar Modo", "bigFont.fnt", "GJ_button_01.png", 0.6f), this, menu_selector(AfterPanel::onMode));
        modeBtn->setPosition({140.f, 140.f}); 
        menu->addChild(modeBtn);
        
        auto* trajBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("Trayectoria ON/OFF", "bigFont.fnt", "GJ_button_04.png", 0.6f), this, menu_selector(AfterPanel::onTraj));
        trajBtn->setPosition({140.f, 90.f}); 
        menu->addChild(trajBtn);

        auto* hbBtn = CCMenuItemSpriteExtra::create(ButtonSprite::create("Hitboxes ON/OFF", "bigFont.fnt", "GJ_button_04.png", 0.6f), this, menu_selector(AfterPanel::onHB));
        hbBtn->setPosition({140.f, 40.f}); 
        menu->addChild(hbBtn);
        
        return true;
    }

    void onMode(CCObject*) {
        std::string m = Mod::get()->getSettingValue<std::string>("assist-mode");
        if (m == "Desactivado") m = "Suave";
        else if (m == "Suave") m = "Balanceado";
        else if (m == "Balanceado") m = "Extremo";
        else m = "Desactivado";
        Mod::get()->setSettingValue<std::string>("assist-mode", m);
        FLAlertLayer::create("Modo", ("Cambiado a: " + m).c_str(), "OK")->show();
    }

    void onTraj(CCObject*) {
        bool t = Mod::get()->getSettingValue<bool>("show-traj");
        Mod::get()->setSettingValue<bool>("show-traj", !t);
    }

    void onHB(CCObject*) {
        bool h = Mod::get()->getSettingValue<bool>("show-hitboxes");
        Mod::get()->setSettingValue<bool>("show-hitboxes", !h);
    }
};

// =====================================================================================
// 3. HOOKS: CAPTURA DE INPUTS Y CICLO DE FÍSICA
// =====================================================================================

class $modify(PlayerHook, PlayerObject) {
    void pushButton(PlayerButton b) {
        if (b != PlayerButton::Jump) { PlayerObject::pushButton(b); return; }
        
        int p = (this == PlayLayer::get()->m_player1) ? 0 : 1;
        AfterEngine::g_holding[p] = true;
        
        if (this->m_isOnGround) {
            if (Mod::get()->getSettingValue<std::string>("assist-mode") == "Desactivado") {
                AfterEngine::tryRewind(this);
            } else {
                AfterEngine::g_bufferedJump[p] = true;
                AfterEngine::g_jumpStartTick[p] = AfterEngine::g_currentTick;
            }
        } else {
            PlayerObject::pushButton(b);
        }
    }

    void releaseButton(PlayerButton b) {
        if (b != PlayerButton::Jump) { PlayerObject::releaseButton(b); return; }
        int p = (this == PlayLayer::get()->m_player1) ? 0 : 1;
        
        AfterEngine::g_holding[p] = false;
        if (AfterEngine::g_bufferedJump[p]) { 
            AfterEngine::g_bufferedJump[p] = false; 
            return; 
        }
        PlayerObject::releaseButton(b);
    }
};

class $modify(BaseHook, GJBaseGameLayer) {
    void update(float dt) {
        AfterEngine::g_currentTick++;
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1) { GJBaseGameLayer::update(dt); return; }

        // Arreglado: Casteo a float en el push_back
        AfterEngine::g_history.push_back({ 
            (float)pl->m_player1->getPositionX(), 
            (float)pl->m_player1->getPositionY(), 
            (float)pl->m_player1->m_yVelocity, 
            pl->m_player1->m_isOnGround, 
            AfterEngine::g_currentTick 
        });
        
        if (AfterEngine::g_history.size() > AfterEngine::MAX_HISTORY) {
            AfterEngine::g_history.pop_front();
        }

        if (Mod::get()->getSettingValue<std::string>("assist-mode") != "Desactivado") {
            AfterEngine::updateHitboxes(pl, pl->m_player1);
            
            for (int i = 0; i < 2; i++) {
                auto* p = (i == 0) ? pl->m_player1 : pl->m_player2;
                if (!p || !AfterEngine::g_bufferedJump[i]) continue;
                
                int maxT = AfterEngine::getMaxTicks(p);
                uint64_t elapsed = AfterEngine::g_currentTick - AfterEngine::g_jumpStartTick[i];
                
                if (p->m_isOnGround) {
                    if (AfterEngine::isSafe(p->getPositionX(), p->getPositionY(), p) || elapsed >= maxT) {
                        p->pushButton(PlayerButton::Jump);
                        AfterEngine::g_bufferedJump[i] = false;
                        if (!AfterEngine::g_holding[i]) p->releaseButton(PlayerButton::Jump);
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
// 4. HOOKS: UI Y DIBUJO
// =====================================================================================

class $modify(MenuHook, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (Mod::get()->getSettingValue<bool>("show-hud")) this->addChild(AfterHUD::create(), 9999);
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
    void openUI(CCObject*) { AfterPanel::create()->show(); }
};

class $modify(PlayHook, PlayLayer) {
    bool init(GJGameLevel* l, bool r, bool d) {
        if (!PlayLayer::init(l, r, d)) return false;
        AfterEngine::g_history.clear();
        if (Mod::get()->getSettingValue<bool>("show-hud")) this->addChild(AfterHUD::create(), 9999);
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

        if (Mod::get()->getSettingValue<bool>("show-hitboxes")) {
            for (size_t i = 0; i < AfterEngine::g_hbCount; i++) {
                auto& hb = AfterEngine::g_hitboxes[i];
                cocos2d::ccColor4F c = hb.isHazard ? cocos2d::ccc4f(1, 0, 0, 0.5f) : cocos2d::ccc4f(0, 1, 0, 0.5f);
                drawNode->drawRect(cocos2d::CCRect(hb.minX, hb.minY, hb.maxX - hb.minX, hb.maxY - hb.minY), c, 1.0f, c);
            }
        }

        if (Mod::get()->getSettingValue<bool>("show-traj")) {
            float px = m_player1->getPositionX(), py = m_player1->getPositionY();
            float vx = 8.4f * m_player1->m_speedMultiplier, vy = AfterEngine::JUMP_VELOCITY;
            bool safe = AfterEngine::isSafe(px, py, m_player1);
            
            auto col = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-primary-color");
            cocos2d::ccColor4F c = safe ? 
                cocos2d::ccc4f(col.r/255.f, col.g/255.f, col.b/255.f, 0.6f) : 
                cocos2d::ccc4f(1, 0, 0, 0.6f); 

            for (int i = 1; i <= 25; ++i) {
                drawNode->drawDot({px + (vx * i), py + (vy * i) - (0.5f * AfterEngine::GRAVITY * i * i)}, 2.5f, c);
            }
        }
    }
};
