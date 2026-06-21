/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#pragma once

struct obs_scene_item;
typedef struct obs_scene_item obs_sceneitem_t;

/* Resets a scene item's transform (position, scale, rotation, alignment,
 * bounds) and crop to their default identity values. No-op when the item is
 * null or locked. */
void ResetSceneItemTransform(obs_sceneitem_t *item);
