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

#include "thing.h"
#include "thing.h"
#include <framework/graphics/graphics.h>
#include "game.h"
#include "map.h"
#include "spritemanager.h"
#include "thingtypemanager.h"
#include "tile.h"

Thing::Thing() : m_datId(0), m_useBlankTexture(false) {}

void Thing::schedulePainting(uint16_t delay)
{
    uint32_t redrawFlag;

    if(isStaticText()) redrawFlag = Otc::ReDrawStaticText;
    else {
        redrawFlag = Otc::ReDrawThing;

        if(isItem()) {
            g_map.schedulePainting(static_cast<Otc::RequestDrawFlags>(redrawFlag), getAnimationInterval());
        } else if(isCreature()) redrawFlag |= Otc::ReDrawAllInformation;

        if(isLocalPlayer() || hasLight()) redrawFlag |= Otc::ReDrawLight;
    }

    g_map.schedulePainting(static_cast<Otc::RequestDrawFlags>(redrawFlag), delay);
}

void Thing::cancelScheduledPainting()
{
    const int delay = getAnimationInterval();
    if(delay == 0) return;

    uint32_t redrawFlag = Otc::ReDrawThing;

    if(isLocalPlayer() || hasLight()) redrawFlag |= Otc::ReDrawLight;
    if(isCreature()) redrawFlag |= Otc::ReDrawAllInformation;

    g_map.cancelScheduledPainting(static_cast<Otc::RequestDrawFlags>(redrawFlag), delay);
}

void Thing::setPosition(const Position& position)
{
    if(m_position == position)
        return;

    const Position oldPos = m_position;
    m_position = position;
    onPositionChange(position, oldPos);
}

int Thing::getStackPriority()
{
    if(isGround())
        return 0;

    if(isGroundBorder())
        return 1;

    if(isOnBottom())
        return 2;

    if(isOnTop())
        return 3;

    if(isCreature())
        return 4;

    // common items
    return 5;
}

const TilePtr& Thing::getTile()
{
    return g_map.getTile(m_position);
}

ContainerPtr Thing::getParentContainer()
{
    if(m_position.x == 0xffff && m_position.y & 0x40) {
        const int containerId = m_position.y ^ 0x40;
        return g_game.getContainer(containerId);
    }

    return nullptr;
}

int Thing::getStackPos()
{
    if(m_position.x == 65535 && isItem()) // is inside a container
        return m_position.z;

    if(const TilePtr& tile = getTile())
        return tile->getThingStackPos(static_self_cast<Thing>());

    g_logger.traceError("got a thing with invalid stackpos");
    return -1;
}

const ThingTypePtr& Thing::getThingType()
{
    return g_things.getNullThingType();
}

ThingType* Thing::rawGetThingType()
{
    return g_things.getNullThingType().get();
}