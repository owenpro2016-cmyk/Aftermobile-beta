#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/GameObject.hpp>
#include <cocos2d.h>
#include <cmath>
#include <string>

using namespace geode::prelude;

// =====================================================================================
// 1. VARIABLES GLOBALES Y FÍSICA
// =====================================================================================

namespace Ticksaver {
    constexpr size_t MAX_CACHED = 64;
    GameObject* g_cachedObstacles[MAX_CACHED];
    size_t g_cachedCount = 0;

    bool g_holding[2] = {false, false};
    bool g_buffered[2] = {false, false}; 
    uint64_t g_bufferStartTick[2] = {0, 0};
    uint64_t g_tick = 0;

    constexpr float GRAVITY = 0.958199f;
    constexpr float JUMP_VELOCITY = 11.180032f;
    constexpr float PLAYER_SIZE = 30.0f;

    void updateCache(PlayLayer* pl, PlayerObject* p1) {
        g_cachedCount = 0;
        if (!pl || !p1 || !pl->m_objects) return;
        float pX = p1->getPositionX();
        float pY = p1->getPositionY();

        for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (g_cachedCount >= MAX_CACHED) break;
            if (!obj) continue;
            auto type = obj->m_objectType;
            if (type == GameObjectType::Solid || type == GameObjectType::Hazard) {
                if (obj->getPositionX() > pX - 100.f && obj->getPositionX() < pX + 400.f &&
                    obj->getPositionY() > pY - 200.f && obj->getPositionY() < pY + 200.f) {
                    g_cachedObstacles[g_cachedCount++] = obj;
                }
            }
        }
    }

    bool checkCollision(float x, float y) {
        for (size_t i = 0; i < g_cachedCount; i++) {
            auto rect = g_cachedObstacles[i]->getObjectRect();
            if (x + (PLAYER_SIZE/2.f) > rect.getMinX() && x - (PLAYER_SIZE/2.f) < rect.getMaxX() &&
                y + (PLAYER_SIZE/2.f) > rect.getMinY() && y - (PLAYER_SIZE/2.f) < rect.getMaxY()) {
                return true;
            }
        }
        return false;
    }

    bool isJumpDeadly(PlayerObject* p1) {
        if (!p1) return false;
        float pX = p1->getPositionX();
        float pY = p1->getPositionY();
        float vX = 8.4f * p1->m_speedMultiplier; 
        float vY = JUMP_VELOCITY; 

        for (int t = 1; t <= 25; ++t) {
            float nextX = pX + (vX * t);
            float nextY = pY + (vY * t) - (0.5f * GRAVITY * t * t);
            if (checkCollision(nextX, nextY)) return true; 
            if (nextY <= pY && t > 5) break; 
        }
        return false;
    }
}

// =====================================================================================
// 2. INTERFAZ GRÁFICA: HUD (Bolita pequeña)
// =====================================================================================

class AssistHUD : public cocos2d::CCLayer {
public:
    static AssistHUD* create() {
        auto* ret = new AssistHUD();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }

    bool init() override {
        if (!CCLayer::init()) return false;

        // Bolita pequeña (radio 18)
        auto* circle = cocos2d::CCDrawNode::create();
        circle->drawCircle({0, 0}, 18.f, cocos2d::ccc4f(0.1f, 0.1f, 0.1f, 0.9f), 2.f, cocos2d::ccc4f(0.f, 1.f, 0.f, 1.f), 32);
        this->addChild(circle);

        auto* label = cocos2d::CCLabelBMFont::create("am", "bigFont.fnt");
        label->setScale(0.6f);
        label->setColor({0, 255, 0});
        this->addChild(label);

        this->setTouchEnabled(true); 
        this->setTouchMode(cocos2d::kCCTouchesOneByOne);
        this->setTouchPriority(-500); 
        this->setPosition({40, cocos2d::CCDirector::get()->getWinSize().height - 80});
        return true;
    }

    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        return this->convertTouchToNodeSpace(touch).getLengthSq() < 400.f;
    }
    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        this->setPosition(this->getPosition() + touch->getDelta());
    }
    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override {
        auto delta = touch->getDelta();
        if (std::abs(delta.x) < 5 && std::abs(delta.y) < 5) {
            // Aquí va la lógica para abrir el menú si la añades después
        }
    }
    void registerWithTouchDispatcher() override {
        cocos2d::CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true);
    }
};

// =====================================================================================
// 3. MOTOR DE INPUTS (TICKSAVER)
// =====================================================================================

class $modify(PlayerObjectHook, PlayerObject) {
    void pushButton(PlayerButton button) {
        auto* pl = PlayLayer::get();
        if (!Mod::get()->getSettingValue<bool>("buffer-perfecto") || button != PlayerButton::Jump || !pl) {
            PlayerObject::pushButton(button);
            return;
        }

        int p = (this == pl->m_player1) ? 0 : 1;
        Ticksaver::g_holding[p] = true;

        if (this->m_isOnGround) {
            Ticksaver::g_buffered[p] = true;
            Ticksaver::g_bufferStartTick[p] = Ticksaver::g_tick;
        } else {
            PlayerObject::pushButton(button);
        }
    }

    void releaseButton(PlayerButton button) {
        auto* pl = PlayLayer::get();
        if (!Mod::get()->getSettingValue<bool>("buffer-perfecto") || button != PlayerButton::Jump || !pl) {
            PlayerObject::releaseButton(button);
            return;
        }

        int p = (this == pl->m_player1) ? 0 : 1;
        Ticksaver::g_holding[p] = false;
        
        if (Ticksaver::g_buffered[p]) {
            Ticksaver::g_buffered[p] = false;
        }
        
        PlayerObject::releaseButton(button);
    }
};

class $modify(InputAssistBase, GJBaseGameLayer) {
    void update(float dt) {
        Ticksaver::g_tick++;
        
        auto* pl = PlayLayer::get();
        if (pl && pl->m_player1 && Mod::get()->getSettingValue<bool>("buffer-perfecto")) {
            Ticksaver::updateCache(pl, pl->m_player1);

            for (int p = 0; p < 2; p++) {
                PlayerObject* player = (p == 0) ? pl->m_player1 : pl->m_player2;
                if (!player) continue;

                if (Ticksaver::g_buffered[p]) {
                    int maxTicks = Mod::get()->getSettingValue<int64_t>("tolerancia-buffer");
                    uint64_t timePassed = Ticksaver::g_tick - Ticksaver::g_bufferStartTick[p];

                    if (player->m_isOnGround) {
                        if (!Ticksaver::isJumpDeadly(player)) {
                            player->pushButton(PlayerButton::Jump);
                            Ticksaver::g_buffered[p] = false;
                            
                            if (!Ticksaver::g_holding[p]) {
                                player->releaseButton(PlayerButton::Jump);
                            }
                        } 
                        else if (timePassed > maxTicks) {
                            Ticksaver::g_buffered[p] = false;
                        }
                    } else {
                        Ticksaver::g_buffered[p] = false;
                    }
                }
            }
        }
        GJBaseGameLayer::update(dt);
    }
};

// =====================================================================================
// 4. HUD EN EL MENÚ Y EN EL JUEGO
// =====================================================================================

// Enganchamos el menú principal para que aparezca la bolita ahí
class $modify(MenuLayerHook, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (Mod::get()->getSettingValue<bool>("mostrar-hud")) {
            this->addChild(AssistHUD::create(), 9999);
        }
        return true;
    }
};

// Enganchamos PlayLayer para que aparezca en el juego
class $modify(InputAssistPlay, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        
        if (Mod::get()->getSettingValue<bool>("mostrar-hud")) {
            this->addChild(AssistHUD::create(), 9999);
        }
        return true;
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
            
            float x = m_player1->getPositionX(); float y = m_player1->getPositionY();
            float vx = 8.4f * m_player1->m_speedMultiplier; float vy = m_player1->m_yVelocity;
            int64_t maxTicks = Mod::get()->getSettingValue<int64_t>("tr-1");
            float thick = Mod::get()->getSettingValue<double>("tr-2");
            
            for (int i = 0; i < maxTicks; ++i) {
                x += vx; if (m_player1->m_isShip) vy += 0.4f; else vy -= 0.958f; y += vy;
                drawNode->drawDot({x, y}, thick, cocos2d::ccc4f(1.0f, 0.2f, 0.2f, 0.5f));
                if (y < 0) break;
            }
        }
        PlayLayer::update(dt);
    }
};
