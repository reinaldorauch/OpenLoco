#include "TileManager.h"
#include "../Input.h"
#include "../Interop/Interop.hpp"
#include "../ViewportManager.h"

using namespace OpenLoco::Interop;

namespace OpenLoco::Map::TileManager
{
    static loco_global<tile_element* [0x30004], 0x00E40134> _tiles;
    static loco_global<coord_t, 0x00F24486> _mapSelectionAX;
    static loco_global<coord_t, 0x00F24488> _mapSelectionBX;
    static loco_global<coord_t, 0x00F2448A> _mapSelectionAY;
    static loco_global<coord_t, 0x00F2448C> _mapSelectionBY;

    constexpr uint16_t mapSelectedTilesSize = 300;
    static loco_global<map_pos[mapSelectedTilesSize], 0x00F24490> _mapSelectedTiles;

    tile get(map_pos pos)
    {
        return get(pos.x, pos.y);
    }

    tile get(coord_t x, coord_t y)
    {
        tile_coord_t tileX = x / 32;
        tile_coord_t tileY = y / 32;

        size_t index = ((y << 9) | x) >> 5;
        auto data = _tiles[index];
        if (data == (tile_element*)0xFFFFFFFF)
        {
            data = nullptr;
        }
        return tile(tileX, tileY, data);
    }

    /**
    * Return the absolute height of an element, given its (x,y) coordinates
    *
    * ax: x
    * cx: y
    * return dx: height
    * return edx >> 16: waterHeight
    * loco: 0x0067297 rct2: 0x00662783 (numbers different)
    */
    std::tuple<int16_t, int16_t> getHeight(coord_t x, coord_t y)
    {
        // Off the map
        if ((unsigned)x >= 12287 || (unsigned)y >= 12287)
            return std::make_tuple(16, 0);

        // Truncate subtile coordinates
        auto xTile = x & 0xFFE0;
        auto yTile = y & 0xFFE0;

        // Get the surface element for the tile
        auto surfaceEl = get(xTile, yTile).surface();

        if (surfaceEl == nullptr)
        {
            return std::make_tuple(16, 0);
        }

        int16_t waterHeight = surfaceEl->water() * 16;
        int16_t height = surfaceEl->baseZ() * 4;

        auto slope = surfaceEl->slopeCorners();
        int8_t quad = 0, quad_extra = 0; // which quadrant the element is in?
                                         // quad_extra is for extra height tiles

        uint8_t TILE_SIZE = 31;

        // Subtile coords
        auto xl = x & 0x1f;
        auto yl = y & 0x1f;

        // Slope logic:
        // Each of the four bits in slope represents that corner being raised
        // slope == 15 (all four bits) is not used and slope == 0 is flat
        // If the extra_height bit is set, then the slope goes up two z-levels

        // We arbitrarily take the SW corner to be closest to the viewer

        // One corner up
        if (slope == SurfaceSlope::n_corner_up || slope == SurfaceSlope::e_corner_up || slope == SurfaceSlope::s_corner_up || slope == SurfaceSlope::w_corner_up)
        {
            switch (slope)
            {
                case SurfaceSlope::n_corner_up:
                    quad = xl + yl - TILE_SIZE;
                    break;
                case SurfaceSlope::e_corner_up:
                    quad = xl - yl;
                    break;
                case SurfaceSlope::s_corner_up:
                    quad = TILE_SIZE - yl - xl;
                    break;
                case SurfaceSlope::w_corner_up:
                    quad = yl - xl;
                    break;
            }
            // If the element is in the quadrant with the slope, raise its height
            if (quad > 0)
            {
                height += quad / 2;
            }
        }

        // One side up
        switch (slope)
        {
            case SurfaceSlope::ne_side_up:
                height += xl / 2 + 1;
                break;
            case SurfaceSlope::se_side_up:
                height += (TILE_SIZE - yl) / 2;
                break;
            case SurfaceSlope::nw_side_up:
                height += yl / 2;
                height++;
                break;
            case SurfaceSlope::sw_side_up:
                height += (TILE_SIZE - xl) / 2;
                break;
        }

        // One corner down
        if ((slope == SurfaceSlope::w_corner_dn) || (slope == SurfaceSlope::s_corner_dn) || (slope == SurfaceSlope::e_corner_dn) || (slope == SurfaceSlope::n_corner_dn))
        {
            switch (slope)
            {
                case SurfaceSlope::w_corner_dn:
                    quad_extra = xl + TILE_SIZE - yl;
                    quad = xl - yl;
                    break;
                case SurfaceSlope::s_corner_dn:
                    quad_extra = xl + yl;
                    quad = xl + yl - TILE_SIZE - 1;
                    break;
                case SurfaceSlope::e_corner_dn:
                    quad_extra = TILE_SIZE - xl + yl;
                    quad = yl - xl;
                    break;
                case SurfaceSlope::n_corner_dn:
                    quad_extra = (TILE_SIZE - xl) + (TILE_SIZE - yl);
                    quad = TILE_SIZE - yl - xl - 1;
                    break;
            }

            if (surfaceEl->isSlopeDoubleHeight())
            {
                height += quad_extra / 2;
                height++;
                return std::make_tuple(height, waterHeight);
            }
            // This tile is essentially at the next height level
            height += 0x10;
            // so we move *down* the slope
            if (quad < 0)
            {
                height += quad / 2;
            }
        }

        // Valleys
        if ((slope == SurfaceSlope::w_e_valley) || (slope == SurfaceSlope::n_s_valley))
        {
            switch (slope)
            {
                case SurfaceSlope::w_e_valley:
                    if (xl + yl <= TILE_SIZE + 1)
                    {
                        return std::make_tuple(height, waterHeight);
                    }
                    quad = TILE_SIZE - xl - yl;
                    break;
                case SurfaceSlope::n_s_valley:
                    quad = xl - yl;
                    break;
            }
            if (quad > 0)
            {
                height += quad / 2;
            }
        }

        return std::make_tuple(height, waterHeight);
    }

    // 0x004610F2
    void mapInvalidateSelectionRect()
    {
        if ((Input::getMapSelectionFlags() & MapSelectFlag::enable) != 0)
        {
            for (coord_t x = _mapSelectionAX; x <= _mapSelectionBX; x += 32)
            {
                for (coord_t y = _mapSelectionAY; y <= _mapSelectionBY; y += 32)
                {
                    mapInvalidateTileFull({ x, y });
                }
            }
        }
    }

    // 0x004CBE5F
    // regs.ax: pos.x
    // regs.cx: pos.y
    void mapInvalidateTileFull(Map::map_pos pos)
    {
        Ui::ViewportManager::invalidate(pos, 0, 1120, ZoomLevel::eighth);
    }

    // 0x0046112C
    void mapInvalidateMapSelectionTiles()
    {
        if ((Input::getMapSelectionFlags() & MapSelectFlag::enableConstruct) == 0)
            return;

        for (uint16_t index = 0; index < mapSelectedTilesSize; ++index)
        {
            auto& position = _mapSelectedTiles[index];
            if (position.x == -1)
                break;
            mapInvalidateTileFull(position);
        }
    }
}
