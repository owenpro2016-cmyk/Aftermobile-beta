#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/ui/ScrollLayer.hpp>
#include <cocos2d.h>
#include <cmath>
#include <vector>
#include <string>
#include <array>

using namespace geode::prelude;

// =====================================================================================
// 1. MOTOR FÍSICO Y DE DATOS (AeroEngine)
// =====================================================================================

namespace AeroEngine {
    constexpr float GRAVITY = 0.958199f;
    constexpr float JUMP_VELOCITY = 11.180032f;
    constexpr float PLAYER_SIZE = 30.0f;

    // Estructuras de Datos
    struct Hitbox { 
        float minX, minY, maxX, maxY; 
        bool isHazard; 
        bool isPortal; 
    };
    
    struct InputLog { 
        uint64_t tick; 
        bool down; 
        int player; 
    };

    // Variables Globales
    std::array<Hitbox, 256> g_cachedHitboxes;
    size_t g_hitboxCount = 0;
    
    std::vector<InputLog> g_replayData;
    std::vector<InputLog> g_clickHistory; // Para "Borrar ultimo click"
    size_t g_replayIndex = 0;
    
    uint64_t g_currentTick = 0;
    bool g_holding[2] = {false, false};
    bool g_bufferedJump[2] = {false, false};
    uint64_t g_jumpStart[2] = {0, 0};

    // Retorna los ticks según la etapa seleccionada
    int getMaxTicks(PlayerObject* p) {
        std::string stage = Mod::get()->getSettingValue<std::string>("assist-stage");
        if (stage == "Desactivado") return 0;
        
        bool isWave = p->m_isDart;
        bool isShip = p->m_isShip;
        
        if (stage == "Suave (1 Tk)") return isWave ? 1 : 2;
        if (stage == "Balanceado (3 Tk)") return isWave ? 3 : 6;
        if (stage == "Extremo (8 Tk)") return isWave ? 8 : 20;
        return 6; // Default
    }

    // --- ESCANEO DE OBJETOS ---
    void updateCache(PlayLayer* pl, PlayerObject* p1) {
        g_hitboxCount = 0;
        if (!pl || !p1 || !pl->m_objects) return;
        float pX = p1->getPositionX(), pY = p1->getPositionY();

        for (auto* obj : CCArrayExt<GameObject*>(pl->m_objects)) {
            if (g_hitboxCount >= g_cachedHitboxes.size()) break;
            if (!obj) continue;
            
            auto type = obj->m_objectType;
            if (type == GameObjectType::Solid || type == GameObjectType::Hazard || type == GameObjectType::Portal) {
                float oX = obj->getPositionX(), oY = obj->getPositionY();
                // Escaneo en un radio de 500px para predecir con anticipación
                if (oX > pX - 150.f && oX < pX + 500.f && oY > pY - 300.f && oY < pY + 300.f) {
                    auto rect = obj->getObjectRect();
                    g_cachedHitboxes[g_hitboxCount++] = { 
                        rect.getMinX(), rect.getMinY(), rect.getMaxX(), rect.getMaxY(), 
                        type == GameObjectType::Hazard, 
                        type == GameObjectType::Portal 
                    };
                }
            }
        }
    }

    // --- SIMULADOR DE COLISIÓN ---
    bool checkCollision(float x, float y) {
        float half = PLAYER_SIZE / 2.0f;
        for (size_t i = 0; i < g_hitboxCount; i++) {
            const auto& hb = g_cachedHitboxes[i];
            if (hb.isPortal) continue; // Los portales no matan
            if (x + half - 2.f > hb.minX && x - half + 2.f < hb.maxX && 
                y + half - 2.f > hb.minY && y - half + 2.f < hb.maxY) {
                return true;
            }
        }
        return false;
    }

    // --- PREDICCIÓN CON PORTALES (Nova Style) ---
    bool isJumpSafe(PlayerObject* p1) {
        if (!p1) return false;
        float pX = p1->getPositionX(), pY = p1->getPositionY();
        float vX = 8.4f * (p1->m_speedMultiplier > 0 ? p1->m_speedMultiplier : 1.0f);
        float vY = JUMP_VELOCITY; 
        
        bool isShip = p1->m_isShip;
        bool isWave = p1->m_isDart;
        bool predictPortals = Mod::get()->getSettingValue<bool>("predict-portals");

        for (int t = 1; t <= 30; ++t) {
            float nextX = pX + (vX * t);
            float nextY = pY + (vY * t) - (0.5f * GRAVITY * t * t);
            
            if (checkCollision(nextX, nextY)) return false; 

            // Si atraviesa un portal, cambia la física de la simulación
            if (predictPortals) {
                for (size_t i = 0; i < g_hitboxCount; i++) {
                    const auto& hb = g_cachedHitboxes[i];
                    if (hb.isPortal && nextX + 15 > hb.minX && nextX - 15 < hb.maxX && nextY + 15 > hb.minY && nextY - 15 < hb.maxY) {
                        isShip = true; // Asumimos que se vuelve nave para predecir
                        isWave = false;
                    }
                }
            }
            
            if (isShip) vY += 0.4f; 
            else if (nextY <= pY && t > 3 && !isWave) break; 
        }
        return true; 
    }
}

// =====================================================================================
// 2. UI: PANEL DE CONTROL IN-GAME (Afterflow Style)
// =====================================================================================

class AeroPanel : public geode::Popup {
protected:
    bool init() override {
        if (!Popup::init(320.f, 260.f)) return false;
        this->setTitle("Aftermobile Control");

        auto colSurface = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-surface-color");
        auto colPrimary = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-primary-color");

        auto* bg = cocos2d::CCLayerColor::create(cocos2d::ccc4(colSurface.r, colSurface.g, colSurface.b, 255), 300.f, 210.f);
        bg->setPosition({10.f, 10.f});
        m_mainLayer->addChild(bg, -1);

        auto* title = CCLabelBMFont::create("PRIME ENGINE", "bigFont.fnt");
        title->setColor(colPrimary);
        title->setScale(0.6f);
        title->setPosition({160.f, 195.f});
        m_mainLayer->addChild(title);

        auto* menu = CCMenu::create();
        menu->setPosition({0, 0});
        m_mainLayer->addChild(menu);

        // Boton 1: Borrar Ultimo Click (Nova Feature)
        auto* delSpr = ButtonSprite::create("Undo Last Click", "bigFont.fnt", "GJ_button_04.png", 0.6f);
        auto* delBtn = CCMenuItemSpriteExtra::create(delSpr, this, menu_selector(AeroPanel::onUndo));
        delBtn->setPosition({160.f, 140.f});
        menu->addChild(delBtn);

        // Boton 2: Limpiar Replay
        auto* clrSpr = ButtonSprite::create("Clear Replay Data", "bigFont.fnt", "GJ_button_06.png", 0.6f);
        auto* clrBtn = CCMenuItemSpriteExtra::create(clrSpr, this, menu_selector(AeroPanel::onClear));
        clrBtn->setPosition({160.f, 85.f});
        menu->addChild(clrBtn);

        // Boton 3: Toggle HUD
        auto* hudSpr = ButtonSprite::create("Toggle HUD", "bigFont.fnt", "GJ_button_01.png", 0.6f);
        auto* hudBtn = CCMenuItemSpriteExtra::create(hudSpr, this, menu_selector(AeroPanel::onToggleHud));
        hudBtn->setPosition({160.f, 30.f});
        menu->addChild(hudBtn);

        return true;
    }

    void onUndo(CCObject*) {
        if (!AeroEngine::g_clickHistory.empty()) {
            AeroEngine::g_clickHistory.pop_back();
            FLAlertLayer::create("AeroEngine", "Ultimo click borrado con exito.", "OK")->show();
        } else {
            FLAlertLayer::create("AeroEngine", "No hay clicks recientes.", "OK")->show();
        }
    }

    void onClear(CCObject*) {
        AeroEngine::g_replayData.clear();
        AeroEngine::g_clickHistory.clear();
        FLAlertLayer::create("AeroEngine", "Memoria limpiada.", "OK")->show();
    }

    void onToggleHud(CCObject*) {
        bool current = Mod::get()->getSettingValue<bool>("show-hud");
        Mod::get()->setSettingValue<bool>("show-hud", !current);
    }

public:
    static AeroPanel* create() {
        auto* ret = new AeroPanel();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }
};

class AeroHUD : public cocos2d::CCLayer {
public:
    static AeroHUD* create() {
        auto* ret = new AeroHUD();
        if (ret->init()) { ret->autorelease(); return ret; }
        delete ret; return nullptr;
    }
    bool init() override {
        if (!CCLayer::init()) return false;
        auto colPrimary = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-primary-color");
        auto c4 = cocos2d::ccc4f(colPrimary.r / 255.f, colPrimary.g / 255.f, colPrimary.b / 255.f, 1.f);
        
        auto* circle = cocos2d::CCDrawNode::create();
        circle->drawCircle({0, 0}, 22.f, cocos2d::ccc4f(0.1f, 0.1f, 0.1f, 0.9f), 2.f, c4, 32);
        circle->setID("am-circle");
        this->addChild(circle);
        
        auto* label = cocos2d::CCLabelBMFont::create("am", "bigFont.fnt");
        label->setScale(0.7f); label->setColor(colPrimary); this->addChild(label);
        
        this->setTouchEnabled(true); this->setTouchMode(cocos2d::kCCTouchesOneByOne);
        this->setTouchPriority(-500); this->setPosition({50, cocos2d::CCDirector::get()->getWinSize().height - 100});
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
        if (std::abs(delta.x) < 5 && std::abs(delta.y) < 5) AeroPanel::create()->show();
    }
    void registerWithTouchDispatcher() override {
        cocos2d::CCDirector::get()->getTouchDispatcher()->addTargetedDelegate(this, -500, true);
    }
};

// =====================================================================================
// 3. INTERCEPCIÓN DE INPUTS (CBF + Replay + Buffer)
// =====================================================================================

class $modify(PlayerObjectHook, PlayerObject) {
    void pushButton(PlayerButton button) {
        if (button != PlayerButton::Jump) { PlayerObject::pushButton(button); return; }
        
        int p = (this == PlayLayer::get()->m_player1) ? 0 : 1;
        std::string stage = Mod::get()->getSettingValue<std::string>("assist-stage");

        // Registrar click para el sistema de "Undo"
        AeroEngine::g_clickHistory.push_back({AeroEngine::g_currentTick, true, p});
        if (Mod::get()->getSettingValue<bool>("enable-replay")) {
            AeroEngine::g_replayData.push_back({AeroEngine::g_currentTick, true, p});
        }

        if (stage != "Desactivado") {
            AeroEngine::g_holding[p] = true;
            if (this->m_isOnGround) {
                AeroEngine::g_bufferedJump[p] = true;
                AeroEngine::g_jumpStart[p] = AeroEngine::g_currentTick;
                return; // Retenemos el salto, no lo ejecutamos aún
            }
        }
        PlayerObject::pushButton(button);
    }

    void releaseButton(PlayerButton button) {
        if (button != PlayerButton::Jump) { PlayerObject::releaseButton(button); return; }
        int p = (this == PlayLayer::get()->m_player1) ? 0 : 1;
        
        AeroEngine::g_holding[p] = false;
        if (AeroEngine::g_bufferedJump[p]) { AeroEngine::g_bufferedJump[p] = false; return; }
        
        if (Mod::get()->getSettingValue<bool>("enable-replay")) {
            AeroEngine::g_replayData.push_back({AeroEngine::g_currentTick, false, p});
        }
        PlayerObject::releaseButton(button);
    }
};

class $modify(InputAssistBase, GJBaseGameLayer) {
    void update(float dt) {
        AeroEngine::g_currentTick++;
        auto* pl = PlayLayer::get();
        if (!pl || !pl->m_player1) { GJBaseGameLayer::update(dt); return; }

        if (Mod::get()->getSettingValue<std::string>("assist-stage") != "Desactivado") {
            AeroEngine::updateCache(pl, pl->m_player1);

            // Reproducción de Replay
            if (Mod::get()->getSettingValue<bool>("enable-replay") && AeroEngine::g_replayIndex < AeroEngine::g_replayData.size()) {
                auto& cmd = AeroEngine::g_replayData[AeroEngine::g_replayIndex];
                if (cmd.tick == AeroEngine::g_currentTick) {
                    PlayerObject* p = (cmd.player == 0) ? pl->m_player1 : pl->m_player2;
                    if (p) { if (cmd.down) p->pushButton(PlayerButton::Jump); else p->releaseButton(PlayerButton::Jump); }
                    AeroEngine::g_replayIndex++;
                }
            }

            // Lógica del Buffer Predictivo
            for (int i = 0; i < 2; i++) {
                PlayerObject* player = (i == 0) ? pl->m_player1 : pl->m_player2;
                if (!player) continue;

                if (AeroEngine::g_bufferedJump[i]) {
                    int maxTicks = AeroEngine::getMaxTicks(player);
                    uint64_t elapsed = AeroEngine::g_currentTick - AeroEngine::g_jumpStart[i];

                    if (player->m_isOnGround) {
                        if (!AeroEngine::isJumpSafe(player) || elapsed >= maxTicks) {
                            player->pushButton(PlayerButton::Jump);
                            AeroEngine::g_bufferedJump[i] = false;
                            if (!AeroEngine::g_holding[i]) player->releaseButton(PlayerButton::Jump);
                        }
                    } else {
                        AeroEngine::g_bufferedJump[i] = false;
                    }
                }
            }
        }
        GJBaseGameLayer::update(dt);
    }
};

// =====================================================================================
// 4. HOOKS DE UI Y DIBUJO (Hitboxes y Trayectoria)
// =====================================================================================

class $modify(MenuLayerHook, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) return false;
        if (Mod::get()->getSettingValue<bool>("show-hud")) this->addChild(AeroHUD::create(), 9999);
        return true;
    }
};

class $modify(PauseLayerHookUI, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto* menu = getChildByID("right-button-menu");
        if (!menu) return;
        auto* btn = CCMenuItemSpriteExtra::create(CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png"), this, menu_selector(PauseLayerHookUI::abrirPanel));
        menu->addChild(btn);
    }
    void abrirPanel(CCObject*) { AeroPanel::create()->show(); }
};

class $modify(InputAssistPlay, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        AeroEngine::g_clickHistory.clear();
        AeroEngine::g_replayData.clear();
        AeroEngine::g_replayIndex = 0;
        if (Mod::get()->getSettingValue<bool>("show-hud")) this->addChild(AeroHUD::create(), 9999);
        return true;
    }

    void draw() {
        PlayLayer::draw();
        if (!m_player1) return;
        
        auto* drawNode = typeinfo_cast<cocos2d::CCDrawNode*>(this->getChildByID("aero-draw-node"));
        if (!drawNode) {
            drawNode = cocos2d::CCDrawNode::create();
            drawNode->setID("aero-draw-node");
            this->addChild(drawNode, 100);
        }
        drawNode->clear();

        // 1. Dibujar Hitboxes (QOLMod Style)
        if (Mod::get()->getSettingValue<bool>("show-hitboxes")) {
            for (size_t i = 0; i < AeroEngine::g_hitboxCount; i++) {
                auto& hb = AeroEngine::g_cachedHitboxes[i];
                if (hb.isPortal) continue;
                cocos2d::ccColor4F color = hb.isHazard ? cocos2d::ccc4f(1, 0, 0, 0.5f) : cocos2d::ccc4f(0, 1, 0, 0.5f);
                drawNode->drawRect({hb.minX, hb.minY}, {hb.maxX, hb.maxY}, color);
            }
        }

        // 2. Dibujar Trayectoria Predictiva (Afterflow/Nova Style)
        if (Mod::get()->getSettingValue<bool>("show-prediction")) {
            float pX = m_player1->getPositionX(), pY = m_player1->getPositionY();
            float vX = 8.4f * m_player1->m_speedMultiplier;
            float vY = AeroEngine::JUMP_VELOCITY;
            bool isSafe = AeroEngine::isJumpSafe(m_player1);
            
            auto colPrimary = Mod::get()->getSettingValue<cocos2d::ccColor3B>("ui-primary-color");
            cocos2d::ccColor4F color = isSafe ? 
                cocos2d::ccc4f(colPrimary.r/255.f, colPrimary.g/255.f, colPrimary.b/255.f, 0.6f) : 
                cocos2d::ccc4f(1, 0, 0, 0.6f);

            for (int i = 1; i <= 25; ++i) {
                float nextX = pX + (vX * i);
                float nextY = pY + (vY * i) - (0.5f * AeroEngine::GRAVITY * i * i);
                drawNode->drawDot({nextX, nextY}, 2.5f, color);
                if (nextY <= pY && i > 3) break;
            }
        }
    }
};
