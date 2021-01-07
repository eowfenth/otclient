/*
 * Copyright (c) 2010-2020 OTClient <https://github.com/edubart/otclient>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "mapview.h"

#include "animatedtext.h"
#include "creature.h"
#include "game.h"
#include "lightview.h"
#include "map.h"
#include "missile.h"
#include "shadermanager.h"
#include "statictext.h"
#include "tile.h"

#include <framework/core/application.h>
#include <framework/core/eventdispatcher.h>
#include <framework/core/resourcemanager.h>
#include <framework/graphics/framebuffermanager.h>
#include <framework/graphics/graphics.h>
#include <framework/graphics/image.h>

enum {
    // 3840x2160 => 1080p optimized
    // 2560x1440 => 720p optimized
    // 1728x972 => 480p optimized

    NEAR_VIEW_AREA = 32 * 32,
    MID_VIEW_AREA = 64 * 64,
    FAR_VIEW_AREA = 128 * 128,
    MAX_TILE_DRAWS = NEAR_VIEW_AREA * 7
};

MapView::MapView()
{
    m_viewMode = NEAR_VIEW;
    m_redrawFlag = Otc::RedrawAll;
    m_lockedFirstVisibleFloor = -1;
    m_cachedFirstVisibleFloor = Otc::SEA_FLOOR;
    m_cachedLastVisibleFloor = Otc::SEA_FLOOR;
    m_minimumAmbientLight = 0;
    m_fadeOutTime = 0;
    m_fadeInTime = 0;
    m_floorMax = 0;
    m_floorMin = 0;

    m_optimizedSize = Size(g_map.getAwareRange().horizontal(), g_map.getAwareRange().vertical()) * Otc::TILE_PIXELS;

    m_frameCache.tile = g_framebuffers.createFrameBuffer();
    m_frameCache.crosshair = g_framebuffers.createFrameBuffer();
    m_frameCache.staticText = g_framebuffers.createFrameBuffer();
    m_frameCache.creatureInformation = g_framebuffers.createFrameBuffer();

    m_shader = g_shaders.getDefaultMapShader();

    m_lastFloorShadowingColor = Color::white;

    setVisibleDimension(Size(15, 11));
    initViewPortDirection();
}

MapView::~MapView()
{
#ifndef NDEBUG
    assert(!g_app.isTerminated());
#endif
}

void MapView::draw(const Rect& rect)
{
    // update visible tiles cache when needed
    if(m_mustUpdateVisibleTilesCache)
        updateVisibleTilesCache();

    const Position cameraPosition = getCameraPosition();

    const auto redrawThing = m_frameCache.tile->canUpdate();
    const auto redrawLight = m_drawLights && m_redrawFlag & Otc::ReDrawLight;

    if(redrawThing || redrawLight) {
        if(redrawLight) {
            Light ambientLight;
            if(cameraPosition.z > Otc::SEA_FLOOR) {
                ambientLight.color = 215;
                ambientLight.intensity = 0;
            } else ambientLight = g_map.getLight();

            ambientLight.intensity = std::max<int>(m_minimumAmbientLight * 255, ambientLight.intensity);
            m_lightView->setGlobalLight(ambientLight);

            m_lightView->reset();
            m_lightView->resize(m_frameCache.tile->getSize());
        }

        m_frameCache.tile->bind();

        if(redrawThing) {
            g_painter->setColor(Color::black);
            g_painter->drawFilledRect(m_rectDimension);
        }

        const auto& lightView = redrawLight ? m_lightView.get() : nullptr;
        const auto& viewPort = isFollowingCreature() && m_followingCreature->isWalking() ? m_viewPortDirection[m_followingCreature->getDirection()] : m_viewPortDirection[Otc::InvalidDirection];

        g_painter->resetColor();
        for(int_fast8_t z = m_floorMax; z >= m_floorMin; --z) {
            onFloorDrawingStart(z);

#if DRAW_ALL_GROUND_FIRST == 1
            drawSeparately(z, viewPort, lightView);
#else
            for(const auto& tile : m_cachedVisibleTiles[z]) {
                const auto hasLight = redrawLight && tile->hasLight();

                if(!redrawThing && !hasLight || !canRenderTile(tile, viewPort, lightView)) continue;

                const Position& tilePos = tile->getPosition();

                tile->drawStart(this);
                tile->draw(transformPositionTo2D(tilePos, cameraPosition), m_scaleFactor, m_redrawFlag, lightView);
                tile->drawEnd(this);
            }
#endif
            for(const MissilePtr& missile : g_map.getFloorMissiles(z)) {
                missile->draw(transformPositionTo2D(missile->getPosition(), cameraPosition), m_scaleFactor, m_redrawFlag, lightView);
            }

            onFloorDrawingEnd(z);
        }

        m_frameCache.tile->release();
    }

    // generating mipmaps each frame can be slow in older cards
    //m_framebuffer->getTexture()->buildHardwareMipmaps();

    float fadeOpacity = 1.0f;
    if(!m_shaderSwitchDone && m_fadeOutTime > 0) {
        fadeOpacity = 1.0f - (m_fadeTimer.timeElapsed() / m_fadeOutTime);
        if(fadeOpacity < 0.0f) {
            m_shader = m_nextShader;
            m_nextShader = nullptr;
            m_shaderSwitchDone = true;
            m_fadeTimer.restart();
        }
    }

    if(m_shaderSwitchDone && m_shader && m_fadeInTime > 0)
        fadeOpacity = std::min<float>(m_fadeTimer.timeElapsed() / m_fadeInTime, 1.0f);

    const Rect srcRect = calcFramebufferSource(rect.size());
    const Point drawOffset = srcRect.topLeft();

    if(m_shader && g_painter->hasShaders() && g_graphics.shouldUseShaders() && m_viewMode == NEAR_VIEW) {
        const Point center = srcRect.center();
        const Point globalCoord = Point(cameraPosition.x - m_drawDimension.width() / 2, -(cameraPosition.y - m_drawDimension.height() / 2)) * m_tileSize;
        m_shader->bind();
        m_shader->setUniformValue(ShaderManager::MAP_CENTER_COORD, center.x / static_cast<float>(m_rectDimension.width()), 1.0f - center.y / static_cast<float>(m_rectDimension.height()));
        m_shader->setUniformValue(ShaderManager::MAP_GLOBAL_COORD, globalCoord.x / static_cast<float>(m_rectDimension.height()), globalCoord.y / static_cast<float>(m_rectDimension.height()));
        m_shader->setUniformValue(ShaderManager::MAP_ZOOM, m_scaleFactor);
        g_painter->setShaderProgram(m_shader);
    }

    g_painter->resetColor();
    g_painter->setOpacity(fadeOpacity);
    glDisable(GL_BLEND);
    m_frameCache.tile->draw(rect, srcRect);
    g_painter->resetShaderProgram();
    g_painter->resetOpacity();
    glEnable(GL_BLEND);

    // this could happen if the player position is not known yet
    if(!cameraPosition.isValid())
        return;

    // Crosshair
    if(m_crosshair.texture && m_crosshair.position.isValid()) {
        if(m_crosshair.positionChanged) {
            m_frameCache.crosshair->bind();
            g_painter->setAlphaWriting(true);
            g_painter->clear(Color::alpha);

            const Point& point = transformPositionTo2D(m_crosshair.position, cameraPosition);
            const Rect crosshairRect = Rect(point * m_scaleFactor, m_crosshair.texture->getWidth(), m_crosshair.texture->getHeight());
            g_painter->drawTexturedRect(crosshairRect, m_crosshair.texture);
            m_frameCache.crosshair->release();

            m_crosshair.positionChanged = false;
        }

        m_frameCache.crosshair->draw(rect, srcRect);
    }

    const float horizontalStretchFactor = rect.width() / static_cast<float>(srcRect.width());
    const float verticalStretchFactor = rect.height() / static_cast<float>(srcRect.height());

    // avoid drawing texts on map in far zoom outs
#if DRAW_CREATURE_INFORMATION_AFTER_LIGHT == 0
    drawCreatureInformation(rect, drawOffset, horizontalStretchFactor, verticalStretchFactor);
#endif

    // lights are drawn after names and before texts
    if(m_drawLights) {
        m_lightView->draw(rect, srcRect);
        m_redrawFlag &= ~Otc::ReDrawLight;
    }

#if DRAW_CREATURE_INFORMATION_AFTER_LIGHT == 1
    drawCreatureInformation(rect, drawOffset, horizontalStretchFactor, verticalStretchFactor);
#endif

    drawText(rect, drawOffset, horizontalStretchFactor, verticalStretchFactor);
}

void MapView::drawCreatureInformation(const Rect& rect, Point drawOffset, const float horizontalStretchFactor, const float verticalStretchFactor)
{
    const bool drawStaticCreatureInf = m_redrawFlag & Otc::ReDrawStaticCreatureInformation;

    if(m_redrawFlag & Otc::ReDrawDynamicCreatureInformation || drawStaticCreatureInf) {
        int flags = 0;
        if(m_drawNames) { flags = Otc::DrawNames; }
        if(m_drawHealthBars) { flags |= Otc::DrawBars; }
        if(m_drawManaBar) { flags |= Otc::DrawManaBar; }

        if(flags) {
            const Position cameraPosition = getCameraPosition();

            m_frameCache.creatureInformation->bind();

            if(drawStaticCreatureInf) {
                g_painter->setAlphaWriting(true);
                g_painter->clear(Color::alpha);
            }

            for(const auto& creature : m_visibleCreatures) {
                if(!creature->canBeSeen())
                    continue;

                // This avoids redesigning the health of the monsters that were not asked for the drawing.
                if(!drawStaticCreatureInf && !creature->updateDynamicInformation()) {
                    continue;
                }

                const auto& tile = creature->getTile();
                if(!tile) continue;

                creature->updateDynamicInformation(false);

                const PointF jumpOffset = creature->getJumpOffset() * m_scaleFactor;
                Point creatureOffset = Point(16 - creature->getDisplacementX(), -creature->getDisplacementY() - 2);
                Position pos = creature->getPosition();
                Point p = transformPositionTo2D(pos, cameraPosition) - drawOffset;
                p += (creature->getDrawOffset() + creatureOffset) * m_scaleFactor - Point(stdext::round(jumpOffset.x), stdext::round(jumpOffset.y));
                p.x *= horizontalStretchFactor;
                p.y *= verticalStretchFactor;
                p += rect.topLeft();

                creature->drawInformation(p, tile->isCovered(), rect, flags);
            }

            m_frameCache.creatureInformation->release();
        }

        m_creatureInfTimeRender.restart();

        m_redrawFlag &= ~Otc::ReDrawStaticCreatureInformation;
        m_redrawFlag &= ~Otc::ReDrawDynamicCreatureInformation;
    }
    m_frameCache.creatureInformation->draw();
}

void MapView::drawText(const Rect& rect, Point drawOffset, const float horizontalStretchFactor, const float verticalStretchFactor)
{
    if(!m_drawTexts) return;

    const Position cameraPosition = getCameraPosition();

    if(!g_map.getStaticTexts().empty()) {
        if(m_redrawFlag & Otc::ReDrawStaticText) {
            m_frameCache.staticText->bind();

            g_painter->setAlphaWriting(true);
            g_painter->clear(Color::alpha);

            for(const StaticTextPtr& staticText : g_map.getStaticTexts()) {
                const Position pos = staticText->getPosition();

                if(pos.z != cameraPosition.z && staticText->getMessageMode() == Otc::MessageNone)
                    continue;

                Point p = transformPositionTo2D(pos, cameraPosition) - drawOffset;
                p.x *= horizontalStretchFactor;
                p.y *= verticalStretchFactor;
                p += rect.topLeft();
                staticText->drawText(p, rect);
            }
            m_frameCache.staticText->release();

            m_redrawFlag &= ~Otc::ReDrawStaticText;
        }

        m_frameCache.staticText->draw();
    }

    for(const AnimatedTextPtr& animatedText : g_map.getAnimatedTexts()) {
        const Position pos = animatedText->getPosition();

        if(pos.z != cameraPosition.z)
            continue;

        Point p = transformPositionTo2D(pos, cameraPosition) - drawOffset;
        p.x *= horizontalStretchFactor;
        p.y *= verticalStretchFactor;
        p += rect.topLeft();

        animatedText->drawText(p, rect);
    }
}

void MapView::updateVisibleTilesCache()
{
    m_mustUpdateVisibleTilesCache = false;

    // there is no tile to render on invalid positions
    const Position cameraPosition = getCameraPosition();
    if(!cameraPosition.isValid())
        return;

    const int cachedFirstVisibleFloor = calcFirstVisibleFloor();
    int cachedLastVisibleFloor = calcLastVisibleFloor();

    assert(cachedFirstVisibleFloor >= 0 && cachedLastVisibleFloor >= 0 &&
           cachedFirstVisibleFloor <= Otc::MAX_Z && cachedLastVisibleFloor <= Otc::MAX_Z);

    if(cachedLastVisibleFloor < cachedFirstVisibleFloor)
        cachedLastVisibleFloor = cachedFirstVisibleFloor;

    // TODO: Review
    /*if(m_cachedFirstVisibleFloor == cachedFirstVisibleFloor &&
       m_cachedLastVisibleFloor == cachedLastVisibleFloor &&
       cameraPosition.z == m_lastCameraPosition.z &&
       cameraPosition.distance(m_lastCameraPosition) < 2
       ) return;*/

    if(m_lastCameraPosition.z != cameraPosition.z) {
        onFloorChange(cameraPosition.z, m_lastCameraPosition.z);
    }

    m_lastCameraPosition = cameraPosition;
    m_cachedFirstVisibleFloor = cachedFirstVisibleFloor;
    m_cachedLastVisibleFloor = cachedLastVisibleFloor;

    // clear current visible tiles cache
    do {
        m_cachedVisibleTiles[m_floorMin].clear();
    } while(++m_floorMin <= m_floorMax);

    uint_fast16_t processedTiles = 0;
    m_floorMin = cameraPosition.z;
    m_floorMax = cameraPosition.z;

    bool stop = false;

    // cache visible tiles in draw order
    // draw from last floor (the lower) to first floor (the higher)
    const int numDiagonals = m_drawDimension.width() + m_drawDimension.height() - 1;
    for(int iz = m_cachedLastVisibleFloor; iz >= m_cachedFirstVisibleFloor && !stop; --iz) {
        auto& floor = m_cachedVisibleTiles[iz];

        // loop through / diagonals beginning at top left and going to top right
        for(int diagonal = 0; diagonal < numDiagonals && !stop; ++diagonal) {
            // loop current diagonal tiles
            const int advance = std::max<int>(diagonal - m_drawDimension.height(), 0);
            for(int iy = diagonal - advance, ix = advance; iy >= 0 && ix < m_drawDimension.width() && !stop; --iy, ++ix) {
                // avoid rendering too much tiles at once
                if(processedTiles > MAX_TILE_DRAWS && m_viewMode >= HUGE_VIEW) {
                    stop = true;
                    break;
                }

                // position on current floor
                //TODO: check position limits
                Position tilePos = cameraPosition.translated(ix - m_virtualCenterOffset.x, iy - m_virtualCenterOffset.y);
                // adjust tilePos to the wanted floor
                tilePos.coveredUp(cameraPosition.z - iz);
                if(const TilePtr& tile = g_map.getTile(tilePos)) {
                    // skip tiles that have nothing
                    if(!tile->isDrawable())
                        continue;

                    // skip tiles that are completely behind another tile
                    if(g_map.isCompletelyCovered(tilePos, m_cachedFirstVisibleFloor))
                        continue;

                    floor.push_back(tile);

                    tile->onVisibleTileList(this);

                    if(iz < m_floorMin)
                        m_floorMin = iz;
                    else if(iz > m_floorMax)
                        m_floorMax = iz;

                    ++processedTiles;
                }
            }
        }
    }
}

void MapView::updateGeometry(const Size& visibleDimension, const Size& optimizedSize)
{
    int tileSize = 0;
    Size bufferSize;

    int possiblesTileSizes[] = { 1,2,4,8,16,32 };
    for(int candidateTileSize : possiblesTileSizes) {
        bufferSize = (visibleDimension + Size(3, 3)) * candidateTileSize;
        if(bufferSize.width() > g_graphics.getMaxTextureSize() || bufferSize.height() > g_graphics.getMaxTextureSize())
            break;

        tileSize = candidateTileSize;
        if(optimizedSize.width() < bufferSize.width() - 3 * candidateTileSize && optimizedSize.height() < bufferSize.height() - 3 * candidateTileSize)
            break;
    }

    if(tileSize == 0) {
        g_logger.traceError("reached max zoom out");
        return;
    }

    Size drawDimension = visibleDimension + Size(3, 3);
    Point virtualCenterOffset = (drawDimension / 2 - Size(1, 1)).toPoint();
    const Point visibleCenterOffset = virtualCenterOffset;

    ViewMode viewMode = m_viewMode;
    if(m_autoViewMode) {

        if(tileSize >= 32 && visibleDimension.area() <= NEAR_VIEW_AREA)
            viewMode = NEAR_VIEW;
        else if(tileSize >= 16 && visibleDimension.area() <= MID_VIEW_AREA)
            viewMode = MID_VIEW;
        else if(tileSize >= 8 && visibleDimension.area() <= FAR_VIEW_AREA)
            viewMode = FAR_VIEW;
        else
            viewMode = HUGE_VIEW;

        m_multifloor = viewMode < FAR_VIEW;
    }

    // draw actually more than what is needed to avoid massive recalculations on huge views
    /* if(viewMode >= HUGE_VIEW) {
        Size oldDimension = drawDimension;
        drawDimension = (m_framebuffer->getSize() / tileSize);
        virtualCenterOffset += (drawDimension - oldDimension).toPoint() / 2;
    }*/

    m_viewMode = viewMode;
    m_visibleDimension = visibleDimension;
    m_drawDimension = drawDimension;
    m_tileSize = tileSize;
    m_virtualCenterOffset = virtualCenterOffset;
    m_visibleCenterOffset = visibleCenterOffset;
    m_optimizedSize = optimizedSize;

    m_rectDimension = Rect(0, 0, m_drawDimension * m_tileSize);

    m_scaleFactor = m_tileSize / static_cast<float>(Otc::TILE_PIXELS);

    m_frameCache.tile->resize(bufferSize);
    m_frameCache.crosshair->resize(bufferSize);

    const Size aboveMapSize = bufferSize * 4;
    m_frameCache.staticText->resize(aboveMapSize);
    m_frameCache.creatureInformation->resize(aboveMapSize);

    resetLastCamera();
    requestVisibleTilesCacheUpdate();
}

void MapView::onFloorChange(const short /*floor*/, const short /*previousFloor*/)
{
    const auto cameraPosition = getCameraPosition();

    if(m_drawLights)
        m_redrawFlag |= Otc::ReDrawLight;

    m_visibleCreatures = g_map.getSpectators(cameraPosition, false);
}

void MapView::onFloorDrawingStart(const short floor)
{
    const auto cameraPosition = getCameraPosition();

    if(m_drawFloorShadowing) {
        Color shadowColor = Color::white;
        if(floor > Otc::SEA_FLOOR && floor != cameraPosition.z) {
            float brightnessLevelStart = .6f;
            float brightnessLevel = cameraPosition.z - floor;
            if(floor > cameraPosition.z)
                brightnessLevel *= -1;
            else brightnessLevelStart -= .1f;

            brightnessLevel *= .12f;

            shadowColor = Color(215, 0, brightnessLevelStart - brightnessLevel);
        } else if(floor < cameraPosition.z) {
            shadowColor = Color((int)g_map.getLight().color, (int)g_map.getLight().intensity / 100, .8f);
        } else if(floor > cameraPosition.z) {
            shadowColor = Color(215, 0, .6f);
        }

        g_painter->setColor(shadowColor);
        m_lastFloorShadowingColor = shadowColor;
    }
}

void MapView::onFloorDrawingEnd(const short /*floor*/)
{
    if(m_drawFloorShadowing) {
        g_painter->resetColor();
    }
}

void MapView::onTileUpdate(const Position& /*pos*/, const ThingPtr& thing, const Otc::Operation operation)
{
    // Need Optimization (update only the specific Tile)
    if(Otc::OPERATION_CLEAN == operation || thing && thing->isLocalPlayer() && Otc::OPERATION_ADD == operation) {
        requestVisibleTilesCacheUpdate();
    }

    if(thing && thing->isCreature() && !thing->isLocalPlayer() && m_lastCameraPosition.z == getCameraPosition().z) {
        const CreaturePtr& creature = thing->static_self_cast<Creature>();
        if(Otc::OPERATION_ADD == operation && isInRange(thing->getPosition())) {
            m_visibleCreatures.push_back(creature);
        } else if(Otc::OPERATION_REMOVE == operation) {
            const auto it = std::find(m_visibleCreatures.begin(), m_visibleCreatures.end(), creature);
            if(it != m_visibleCreatures.end())
                m_visibleCreatures.erase(it);
        }
    }
}

void MapView::onMapCenterChange(const Position&)
{
    requestVisibleTilesCacheUpdate();
}

void MapView::lockFirstVisibleFloor(int firstVisibleFloor)
{
    m_lockedFirstVisibleFloor = firstVisibleFloor;
    requestVisibleTilesCacheUpdate();
}

void MapView::unlockFirstVisibleFloor()
{
    m_lockedFirstVisibleFloor = -1;
    requestVisibleTilesCacheUpdate();
}

void MapView::setVisibleDimension(const Size& visibleDimension)
{
    if(visibleDimension == m_visibleDimension)
        return;

    if(visibleDimension.width() % 2 != 1 || visibleDimension.height() % 2 != 1) {
        g_logger.traceError("visible dimension must be odd");
        return;
    }

    if(visibleDimension < Size(3, 3)) {
        g_logger.traceError("reach max zoom in");
        return;
    }

    updateGeometry(visibleDimension, m_optimizedSize);
}

void MapView::setViewMode(ViewMode viewMode)
{
    m_viewMode = viewMode;
    requestVisibleTilesCacheUpdate();
}

void MapView::setAutoViewMode(bool enable)
{
    m_autoViewMode = enable;
    if(enable)
        updateGeometry(m_visibleDimension, m_optimizedSize);
}

void MapView::optimizeForSize(const Size& visibleSize)
{
    updateGeometry(m_visibleDimension, visibleSize);
}

void MapView::followCreature(const CreaturePtr& creature)
{
    m_follow = true;
    m_followingCreature = creature;
    requestVisibleTilesCacheUpdate();
}

void MapView::setCameraPosition(const Position& pos)
{
    m_follow = false;
    m_customCameraPosition = pos;
    requestVisibleTilesCacheUpdate();
}

Position MapView::getPosition(const Point& point, const Size& mapSize)
{
    const Position cameraPosition = getCameraPosition();

    // if we have no camera, its impossible to get the tile
    if(!cameraPosition.isValid())
        return Position();

    const Rect srcRect = calcFramebufferSource(mapSize);
    const float sh = srcRect.width() / static_cast<float>(mapSize.width());
    const float sv = srcRect.height() / static_cast<float>(mapSize.height());

    const Point framebufferPos = Point(point.x * sh, point.y * sv);
    const Point centerOffset = (framebufferPos + srcRect.topLeft()) / m_tileSize;

    const Point tilePos2D = getVisibleCenterOffset() - m_drawDimension.toPoint() + centerOffset + Point(2, 2);
    if(tilePos2D.x + cameraPosition.x < 0 && tilePos2D.y + cameraPosition.y < 0)
        return Position();

    Position position = Position(tilePos2D.x, tilePos2D.y, 0) + cameraPosition;

    if(!position.isValid())
        return Position();

    return position;
}

void MapView::move(int x, int y)
{
    m_moveOffset.x += x;
    m_moveOffset.y += y;

    int32_t tmp = m_moveOffset.x / 32;
    bool requestTilesUpdate = false;
    if(tmp != 0) {
        m_customCameraPosition.x += tmp;
        m_moveOffset.x %= 32;
        requestTilesUpdate = true;
    }

    tmp = m_moveOffset.y / 32;
    if(tmp != 0) {
        m_customCameraPosition.y += tmp;
        m_moveOffset.y %= 32;
        requestTilesUpdate = true;
    }

    if(requestTilesUpdate)
        requestVisibleTilesCacheUpdate();
}

Rect MapView::calcFramebufferSource(const Size& destSize)
{
    Point drawOffset = ((m_drawDimension - m_visibleDimension - Size(1, 1)).toPoint() / 2) * m_tileSize;
    if(isFollowingCreature())
        drawOffset += m_followingCreature->getWalkOffset() * m_scaleFactor;
    else if(!m_moveOffset.isNull())
        drawOffset += m_moveOffset * m_scaleFactor;

    Size srcSize = destSize;
    const Size srcVisible = m_visibleDimension * m_tileSize;
    srcSize.scale(srcVisible, Fw::KeepAspectRatio);
    drawOffset.x += (srcVisible.width() - srcSize.width()) / 2;
    drawOffset.y += (srcVisible.height() - srcSize.height()) / 2;

    return Rect(drawOffset, srcSize);
}

int MapView::calcFirstVisibleFloor()
{
    int z = Otc::SEA_FLOOR;
    // return forced first visible floor
    if(m_lockedFirstVisibleFloor != -1) {
        z = m_lockedFirstVisibleFloor;
    } else {
        const Position cameraPosition = getCameraPosition();

        // this could happens if the player is not known yet
        if(cameraPosition.isValid()) {
            // avoid rendering multifloors in far views
            if(!m_multifloor) {
                z = cameraPosition.z;
            } else {
                // if nothing is limiting the view, the first visible floor is 0
                int firstFloor = 0;

                // limits to underground floors while under sea level
                if(cameraPosition.z > Otc::SEA_FLOOR)
                    firstFloor = std::max<int>(cameraPosition.z - Otc::AWARE_UNDEGROUND_FLOOR_RANGE, static_cast<int>(Otc::UNDERGROUND_FLOOR));

                // loop in 3x3 tiles around the camera
                for(int ix = -1; ix <= 1 && firstFloor < cameraPosition.z; ++ix) {
                    for(int iy = -1; iy <= 1 && firstFloor < cameraPosition.z; ++iy) {
                        const Position pos = cameraPosition.translated(ix, iy);

                        // process tiles that we can look through, e.g. windows, doors
                        if((ix == 0 && iy == 0) || ((std::abs(ix) != std::abs(iy)) && g_map.isLookPossible(pos))) {
                            Position upperPos = pos;
                            Position coveredPos = pos;

                            const auto isLookPossible = g_map.isLookPossible(pos);
                            while(coveredPos.coveredUp() && upperPos.up() && upperPos.z >= firstFloor) {
                                // check tiles physically above
                                TilePtr tile = g_map.getTile(upperPos);
                                if(tile && tile->limitsFloorsView(!isLookPossible)) {
                                    firstFloor = upperPos.z + 1;
                                    break;
                                }

                                // check tiles geometrically above
                                tile = g_map.getTile(coveredPos);
                                if(tile && tile->limitsFloorsView(isLookPossible)) {
                                    firstFloor = coveredPos.z + 1;
                                    break;
                                }
                            }
                        }
                    }
                }

                z = firstFloor;
            }
        }
    }

    // just ensure the that the floor is in the valid range
    z = stdext::clamp<int>(z, 0, static_cast<int>(Otc::MAX_Z));
    return z;
}

int MapView::calcLastVisibleFloor()
{
    if(!m_multifloor)
        return calcFirstVisibleFloor();

    int z = Otc::SEA_FLOOR;

    const Position cameraPosition = getCameraPosition();
    // this could happens if the player is not known yet
    if(cameraPosition.isValid()) {
        // view only underground floors when below sea level
        if(cameraPosition.z > Otc::SEA_FLOOR)
            z = cameraPosition.z + Otc::AWARE_UNDEGROUND_FLOOR_RANGE;
        else
            z = Otc::SEA_FLOOR;
    }

    if(m_lockedFirstVisibleFloor != -1)
        z = std::max<int>(m_lockedFirstVisibleFloor, z);

    // just ensure the that the floor is in the valid range
    z = stdext::clamp<int>(z, 0, static_cast<int>(Otc::MAX_Z));
    return z;
}

Position MapView::getCameraPosition()
{
    if(isFollowingCreature())
        return m_followingCreature->getPosition();

    return m_customCameraPosition;
}

void MapView::setShader(const PainterShaderProgramPtr& shader, float fadein, float fadeout)
{
    if((m_shader == shader && m_shaderSwitchDone) || (m_nextShader == shader && !m_shaderSwitchDone))
        return;

    if(fadeout > 0.0f && m_shader) {
        m_nextShader = shader;
        m_shaderSwitchDone = false;
    } else {
        m_shader = shader;
        m_nextShader = nullptr;
        m_shaderSwitchDone = true;
    }
    m_fadeTimer.restart();
    m_fadeInTime = fadein;
    m_fadeOutTime = fadeout;
}

void MapView::setDrawLights(bool enable)
{
    if(enable == m_drawLights) return;

    m_lightView = enable ? LightViewPtr(new LightView) : nullptr;
    m_drawLights = enable;

    schedulePainting(Otc::RedrawAll);
}

void MapView::initViewPortDirection()
{
    const AwareRange& awareRange = g_map.getAwareRange();
    for(int dir = Otc::North; dir <= Otc::InvalidDirection; ++dir) {
        ViewPort& vp = m_viewPortDirection[dir];
        vp.top = awareRange.top;
        vp.right = awareRange.right;
        vp.bottom = vp.top;
        vp.left = vp.right;

        switch(dir) {
        case Otc::North:
        case Otc::South:
            vp.top += 1;
            vp.bottom += 1;
            break;

        case Otc::West:
        case Otc::East:
            vp.right += 1;
            vp.left += 1;
            break;

        case Otc::NorthEast:
        case Otc::SouthEast:
        case Otc::NorthWest:
        case Otc::SouthWest:
            vp.left += 1;
            vp.bottom += 1;
            vp.top += 1;
            vp.right += 1;
            break;

        case Otc::InvalidDirection:
            vp.left -= 1;
            vp.right -= 1;
            break;

        default:
            break;
        }
    }
}

bool MapView::canRenderTile(const TilePtr& tile, const ViewPort& viewPort, LightView* lightView)
{
    if(m_drawViewportEdge || lightView && lightView->isDark() && tile->hasLight()) return true;

    const Position cameraPosition = getCameraPosition();
    const Position& tilePos = tile->getPosition();

    const int dz = tilePos.z - cameraPosition.z;
    const Position checkPos = tilePos.translated(dz, dz);

    // Check for non-visible tiles on the screen and ignore them
    {
        if((cameraPosition.x - checkPos.x >= viewPort.left) || (checkPos.x - cameraPosition.x == viewPort.right && !tile->hasWideThings() && !tile->hasDisplacement()))
            return false;

        if((cameraPosition.y - checkPos.y >= viewPort.top) || (checkPos.y - cameraPosition.y == viewPort.bottom && !tile->hasTallThings() && !tile->hasDisplacement()))
            return false;

        if((checkPos.x - cameraPosition.x > viewPort.right && (!tile->hasWideThings() || !tile->hasDisplacement())) || (checkPos.y - cameraPosition.y > viewPort.bottom))
            return false;
    }

    return true;
}

void MapView::schedulePainting(const Otc::RequestDrawFlags reDrawFlags, uint16_t delay)
{
    if(reDrawFlags & Otc::ReDrawStaticText) {
        m_frameCache.staticText->update();
        return;
    }

    if(reDrawFlags & Otc::ReDrawThing) {
        m_frameCache.tile->addRenderingTime(delay);
    }

    if(reDrawFlags & Otc::ReDrawCreatureInformation || reDrawFlags & Otc::ReDrawDynamicCreatureInformation) {
        m_frameCache.creatureInformation->addRenderingTime(delay);
    }

    /*if(m_drawLights && reDrawFlags & Otc::ReDrawLight) {
        if(Otc::OPERATION_ADD)
            m_lightView->addRenderingTime(delay);
        else m_frameCache.tile->removeRenderingTime(delay);
    }*/
}

void MapView::cancelScheduledPainting(const Otc::RequestDrawFlags reDrawFlags, uint16_t delay)
{
    if(reDrawFlags & Otc::ReDrawThing) {
        m_frameCache.tile->removeRenderingTime(delay);
    }
}

bool MapView::isInRange(const Position& pos)
{
    const Position camera = getCameraPosition();

    if(camera.z != m_lastCameraPosition.z) return false;

    const AwareRange& awareRange = g_map.getAwareRange();
    return camera.isInRange(pos, awareRange.left, awareRange.right, awareRange.top, awareRange.bottom);
}

void MapView::setCrosshairPosition(const Position& pos)
{
    if(pos == m_crosshair.position) return;

    m_crosshair.position = pos;
    m_crosshair.positionChanged = true;
}
void MapView::setCrosshairTexture(const std::string& texturePath)
{
    m_crosshair.texture = texturePath.empty() ? nullptr : g_textures.getTexture(texturePath);
}

#if DRAW_ALL_GROUND_FIRST == 1
void MapView::drawSeparately(const int floor, const ViewPort& viewPort, LightView* lightView)
{
    const Position cameraPosition = getCameraPosition();
    const auto& tiles = m_cachedVisibleTiles[floor];
    const auto redrawThing = m_redrawFlag & Otc::ReDrawThing;
    const auto redrawLight = m_drawLights && m_redrawFlag & Otc::ReDrawLight;

    for(const auto& tile : tiles) {
        if(!tile->hasGroundToDraw()) continue;

        const auto hasLight = redrawLight && tile->hasLight();

        if(!redrawThing && !hasLight || !canRenderTile(tile, viewPort, lightView)) continue;

        const Position& tilePos = tile->getPosition();
        tile->drawStart(this);
        tile->drawGround(transformPositionTo2D(tilePos, cameraPosition), m_scaleFactor, m_redrawFlag, lightView);
        tile->drawEnd(this);
    }

    for(const auto& tile : tiles) {
        if(!tile->hasBottomToDraw() && !tile->hasTopToDraw()) continue;

        const auto hasLight = redrawLight && tile->hasLight();

        if(!redrawThing && !hasLight || !canRenderTile(tile, viewPort, lightView)) continue;

        const Position& tilePos = tile->getPosition();

        const Point pos2d = transformPositionTo2D(tilePos, cameraPosition);

        if(!tile->hasGroundToDraw()) tile->drawStart(this);

        tile->drawBottom(pos2d, m_scaleFactor, m_redrawFlag, lightView);
        tile->drawTop(pos2d, m_scaleFactor, m_redrawFlag, lightView);

        if(!tile->hasGroundToDraw()) tile->drawEnd(this);
    }
}
#endif
/* vim: set ts=4 sw=4 et: */