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
// 1. VARIABLES GLOBALES Y CONFIGURACIÓN
// =====================================================================================

namespace BufferTAS {
    // Caché de obstáculos cercanos para no calcular todo el nivel (optimización extrema)
    constexpr size_t MAX_CACHED = 64;
    GameObject* g_cachedObstacles[MAX_CACHED];
    size_t g_cachedCount = 0;

    // Estado del jugador
    bool g_isHolding[2] = {false, false};
    bool g_jumpBuffered[2] = {false, false}; 
    uint64_t g_bufferTick[2] = {0, 0};
    uint64_t g_currentTick = 0;
    
    // Constantes de física real de GD
    constexpr float GRAVITY = 0.958199f;
    constexpr float JUMP_VELOCITY = 11.180032f;
    constexpr float PLAYER_SIZE = 30.0f;

    // --- ESCANEO LOCAL DE OBSTÁCULOS ---
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
                float oX = obj->getPositionX();
                float oY = obj->getPositionY();
                // Solo guardar lo que está a 400px a la redonda (aprox 2 segundos)
                if (oX > pX - 100.f && oX < pX + 400.f && oY > pY - 200.f && oY < pY + 200.f) {
                    g_cachedObstacles[g_cachedCount++] = obj;
                }
            }
        }
    }

    // --- COMPROBAR COLISIÓN EN UN PUNTO X/Y ---
    bool checkCollision(float x, float y) {
        for (size_t i = 0; i < g_cachedCount; i++) {
            auto* obj = g_cachedObstacles[i];
            auto rect = obj->getObjectRect();
            // Si la hitbox del jugador (30x30) toca el obstáculo
            if (x + (PLAYER_SIZE/2.f) > rect.getMinX() && x - (PLAYER_SIZE/2.f) < rect.getMaxX() &&
                y + (PLAYER_SIZE/2.f) > rect.getMinY() && y - (PLAYER_SIZE/2.f) < rect.getMaxY()) {
                return true;
            }
        }
        return false;
    }

    // --- SIMULADOR DE SALTO PERFECTO ---
    // Devuelve true si saltar en este tick mata al jugador (ya sea por pincho o por techo)
    bool isJumpDeadly(PlayerObject* p1) {
        if (!p1) return false;
        
        float pX = p1->getPositionX();
        float pY = p1->getPositionY();
        float vX = 8.4f * p1->m_speedMultiplier; 
        float vY = JUMP_VELOCITY; 

        // Simulamos el arco del salto durante 25 ticks (los suficientes para esquivar cualquier pincho)
        for (int t = 1; t <= 25; ++t) {
            float nextX = pX + (vX * t);
            float nextY = pY + (vY * t) - (0.5f * GRAVITY * t * t);
            
            // Si en algún punto del salto choca con un sólido/pincho, el salto es mortal.
            if (checkCollision(nextX, nextY)) {
                return true; 
            }
            
            // Si ya cayó al suelo, el salto terminó seguro.
            if (nextY <= pY && t > 5) break; 
        }
        return false; // Es seguro saltar
    }
}

// =====================================================================================
// 2. LÓGICA DEL JUEGO (EL "COLADOR" DE CLICKS)
// =====================================================================================

class $modify(InputAssistBase, GJBaseGameLayer) {
    void update(float dt) {
        BufferTAS::g_currentTick++;
        
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1) {
            GJBaseGameLayer::update(dt);
            return;
        }

        bool bufferEnabled = Mod::get()->getSettingValue<bool>("buffer-perfecto");
        
        if (bufferEnabled) {
            BufferTAS::updateCache(pl, pl->m_player1);

            for (int p = 0; p < 2; p++) {
                PlayerObject* player = (p == 0) ? pl->m_player1 : pl->m_player2;
                if (!player) continue;

                // Si hay un click en la memoria esperando ser ejecutado
                if (BufferTAS::g_jumpBuffered[p]) {
                    int maxTicks = Mod::get()->getSettingValue<int64_t>("tolerancia-clicks");
                    uint64_t timePassed = BufferTAS::g_currentTick - BufferTAS::g_bufferTick[p];

                    if (player->m_isOnGround) {
                        // Si el salto es SEGURO, ¡lo ejecutamos!
                        if (!BufferTAS::isJumpDeadly(player)) {
                            player->pushButton(PlayerButton::Jump);
                            BufferTAS::g_jumpBuffered[p] = false;
                            
                            // Si el jugador ya había soltado la pantalla, hacemos un "tap" rápido
                            if (!BufferTAS::g_isHolding[p]) {
                                player->releaseButton(PlayerButton::Jump);
                            }
                        } 
                        // Si el salto es mortal, esperamos al siguiente tick.
                        // Pero si se acabó el tiempo de tolerancia, descartamos el click (para no hacer saltos fantasma).
                        else if (timePassed > maxTicks) {
                            BufferTAS::g_jumpBuffered[p] = false;
                        }
                    } 
                    else {
                        // En el aire (Orbes). Aquí simulamos si hay un orbe cerca para activarlo.
                        // Simplificación: si está en el aire y clickea, el juego lo procesa normal.
                        BufferTAS::g_jumpBuffered[p] = false; 
                    }
                }
            }
        }

        GJBaseGameLayer::update(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (!Mod::get()->getSettingValue<bool>("buffer-perfecto")) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        int p = isPlayer1 ? 0 : 1;
        auto* pl = PlayLayer::get();
        if (!pl) return;
        PlayerObject* player = isPlayer1 ? pl->m_player1 : pl->m_player2;
        if (!player) return;

        if (down) {
            BufferTAS::g_isHolding[p] = true;
            
            // Si está en el suelo, no saltamos aún. Lo mandamos a la memoria.
            if (player->m_isOnGround) {
                BufferTAS::g_jumpBuffered[p] = true;
                BufferTAS::g_bufferTick[p] = BufferTAS::g_currentTick;
            } 
            // Si está en el aire (nave, wave, orbes), el input es directo.
            else {
                player->pushButton(PlayerButton::Jump);
            }
        } else {
            BufferTAS::g_isHolding[p] = false;
            BufferTAS::g_jumpBuffered[p] = false; // Cancelar la memoria si suelta
            player->releaseButton(PlayerButton::Jump);
        }
    }
};

// =====================================================================================
// 3. UI (MENÚ Y HUD)
// =====================================================================================

class AssistPopup : public geode::Popup {
protected:
    cocos2d::CCLayer* m_contentLayer = nullptr;
    void createToggle(cocos2d::CCLayer* parent, std::string id, std::string text, cocos2d::CCPoint pos) {
        auto* label = CCLabelBMFont::create(text.c_str(), "bigFont.fnt"); label->setScale(0.35f); label->setAnchorPoint({0.f, 0.5f}); label->setPosition(pos); parent->addChild(label);
        auto* toggler = CCMenuItemToggler::create(CCSprite::createWithSpriteFrameName("GJ_checkOff_001.png"), CCSprite::createWithSpriteFrameName("GJ_checkOn_001.png"), this, menu_selector(AssistPopup::onToggle));
        toggler->setPosition({pos.x + 200.f, pos.y}); toggler->setID(id); toggler->toggle(Mod::get()->getSettingValue<bool>(id));
        auto* menu = CCMenu::create(); menu->setPosition({0, 0}); menu->addChild(toggler); parent->addChild(menu);
    }
    void onToggle(CCObject* sender) { auto* t = typeinfo_cast<CCMenuItemToggler*>(sender); if (t) Mod::get()->setSettingValue<bool>(t->getID(), t->isToggled()); }
    
    bool init() override {
        if (!Popup::init(300.f, 280.f)) return false;
        this->setTitle("Aftermobile TAS");
        auto* bg = cocos2d::CCLayerColor::create(cocos2d::ccc4(10, 20, 10, 255), 280.f, 240.f); bg->setPosition({10.f, 10.f}); m_mainLayer->addChild(bg, -1);
        m_contentLayer = cocos2d::CCLayer::create(); m_mainLayer->addChild(m_contentLayer);
        createToggle(m_contentLayer, "buffer-perfecto", "Buffer Perfecto TAS", {20, 180});
        createToggle(m_contentLayer, "mostrar-trayectoria", "Mostrar Trayectoria", {20, 140});
        return true;
    }
public:
    static AssistPopup* create() { auto* ret = new AssistPopup(); if (ret->init()) { ret->autorelease(); return ret; } delete ret; return nullptr; }
};

class AssistHUD : public cocos2d::CCLayer {
public:
    cocos2d::CCLabelBMFont* m_label = nullptr;
    static AssistHUD* create() { auto* ret = new AssistHUD(); if (ret->init()) { ret->autorelease(); return ret; } delete ret; return nullptr; }
    bool init() override {
        if (!CCLayer::init()) return false;
        auto* circle = cocos2d::CCDrawNode::create();
        circle->drawCircle({0, 0}, 28.f, cocos2d::ccc4f(0.1f, 0.1f, 0.1f, 1.f), 3.f, cocos2d::ccc4f(0.f, 1.f, 0.f, 1.f), 32);
        this->addChild(circle);
        m_label = cocos2d::CCLabelBMFont::create("am", "bigFont.fnt"); m_label->setScale(0.9f); m_label->setColor({0, 255, 0}); this->addChild(m_label);
        this->setTouchEnabled(true); this->setTouchMode(cocos2d::kCCTouchesOneByOne); this->setTouchPriority(-500); 
        this->setPosition({50, cocos2d::CCDirector::get()->getWinSize().height - 100});
        return true;
    }
    bool ccTouchBegan(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override { return this->convertTouchToNodeSpace(touch).getLengthSq() < 900.f; }
    void ccTouchMoved(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override { this->setPosition(this->getPosition() + touch->getDelta()); }
    void ccTouchEnded(cocos2d::CCTouch* touch, cocos2d::CCEvent* event) override { if (std::abs(touch->getDelta().x) < 5) AssistPopup::create()->show(); }
    void registerWithTouchDispatcher() override { cocos2d::CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true); }
};

class $modify(InputAssistPlay, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        static bool hudCreated = false;
        if (!hudCreated) { this->addChild(AssistHUD::create(), 9999); hudCreated = true; }
        return true;
    }
    void update(float dt) {
        if (Mod::get()->getSettingValue<bool>("mostrar-trayectoria") && m_player1) {
            auto* drawNode = typeinfo_cast<cocos2d::CCDrawNode*>(this->getChildByID("trajectory-draw-node"));
            if (!drawNode) { drawNode = cocos2d::CCDrawNode::create(); drawNode->setID("trajectory-draw-node"); this->addChild(drawNode, 100); }
            drawNode->clear();
            float x = m_player1->getPositionX(); float y = m_player1->getPositionY();
            float vx = 8.4f * m_player1->m_speedMultiplier; float vy = m_player1->m_yVelocity;
            int64_t maxTicks = Mod::get()->getSettingValue<int64_t>("tr-1");
            for (int i = 0; i < maxTicks; ++i) {
                x += vx; if (m_player1->m_isShip) vy += 0.4f; else vy -= 0.958f; y += vy;
                drawNode->drawDot({x, y}, 2.5f, cocos2d::ccc4f(1.0f, 0.2f, 0.2f, 0.5f));
                if (y < 0) break;
            }
        }
        PlayLayer::update(dt);
    }
};

class $modify(PauseLayerHookUI, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto* menu = getChildByID("right-button-menu"); if (!menu) return;
        menu->addChild(CCMenuItemSpriteExtra::create(CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png"), this, menu_selector(PauseLayerHookUI::abrirPanel)));
    }
    void abrirPanel(CCObject*) { AssistPopup::create()->show(); }
};
