#include "spatial_grid.h"

#include "gettime.h"
#include "collision.h"
#include "instance.h"
#include "runner.h"
#include "utils.h"

SpatialGrid* SpatialGrid_create(uint32_t roomWidth, uint32_t roomHeight) {
    SpatialGrid* grid = (SpatialGrid *)safeCalloc(1, sizeof(SpatialGrid));

    // +1 to avoid truncation
    uint32_t gridWidth = (roomWidth / SPATIAL_GRID_CELL_SIZE) + 1;
    uint32_t gridHeight = (roomHeight / SPATIAL_GRID_CELL_SIZE) + 1;

#ifdef ENABLE_SPATIAL_GRID_LOGS
    logInfo("SpatialGrid: Grid size: %dx%d\n", gridWidth, gridHeight);
#endif
    grid->gridWidth = gridWidth;
    grid->gridHeight = gridHeight;

    grid->grid = (Instance ***)safeCalloc(gridWidth * gridHeight, sizeof(Instance**));

    return grid;
}

static long g_gridRemoves = 0, g_gridFullScans = 0, g_gridCellsVisited = 0, g_gridEntriesScanned = 0, g_gridMovedBytes = 0; // TEMP
void SpatialGrid_free(SpatialGrid* grid) {
    fprintf(stderr, "GRIDSTAT removes=%ld fullScans=%ld cellsVisited=%ld entriesScanned=%ld movedBytes=%ld\n", g_gridRemoves, g_gridFullScans, g_gridCellsVisited, g_gridEntriesScanned, g_gridMovedBytes); // TEMP
    int32_t totalCells = grid->gridWidth * grid->gridHeight;
    repeat(totalCells, i) {
        arrfree(grid->grid[i]);
    }
    free(grid->grid);
    arrfree(grid->dirtyInstances);
    free(grid);
}

// Repair the slot record of an instance displaced by swap-remove: it moved to newSlot
// within the cell identified by packed gridCoordinates. Best-effort: if the instance
// doesn't track this cell (pre-existing inconsistency), its slot just stays stale and
// its own removal falls back to the linear sweep, so correctness never depends on it.
static void SpatialGrid_fixMovedSlot(Instance* moved, uint32_t gridCoordinates, int32_t newSlot) {
    int32_t movedCells = (int32_t) arrlen(moved->collisionCells);
    int32_t movedSlots = (int32_t) arrlen(moved->collisionCellSlots);
    repeat(movedCells, k) {
        if (k < movedSlots && (uint32_t) moved->collisionCells[k] == gridCoordinates) {
            moved->collisionCellSlots[k] = newSlot;
            return;
        }
    }
}

void SpatialGrid_removeInstance(SpatialGrid* grid, Instance* instance) {
    g_gridRemoves++; // TEMP
    int32_t totalCells = (int32_t)grid->gridWidth * (int32_t)grid->gridHeight;
    int32_t trackedCellCount = (int32_t) arrlen(instance->collisionCells);
    int32_t removedCount = 0;

    // The instance records every cell used when it is inserted, so the normal
    // removal path only needs to visit those cells. This matters for moving
    // danmaku: hundreds of bullets and graze boxes are re-indexed every frame.
    // Each tracked cell also records the instance's index within that cell, so
    // removal is O(1) instead of a linear scan plus memmove. The recorded slot
    // is always verified before use, so a stale slot can only cost a fallback
    // scan, never a wrong removal. (Cell order is not preserved by swap-remove;
    // collision queries don't depend on it.)
    int32_t slotCount = (int32_t) arrlen(instance->collisionCellSlots);
    repeat(trackedCellCount, i) {
        uint32_t gridCoordinates = instance->collisionCells[i];
        int32_t gridX = SpatialGrid_unpackGridX(gridCoordinates);
        int32_t gridY = SpatialGrid_unpackGridY(gridCoordinates);
        int32_t cellIndex = SpatialGrid_cellIndex(grid, gridX, gridY);
        if (cellIndex < 0 || cellIndex >= totalCells) continue;

        Instance** cell = grid->grid[cellIndex];
        int32_t cellLen = (int32_t) arrlen(cell);
        g_gridCellsVisited++; // TEMP
        int32_t recSlot = (i < slotCount) ? instance->collisionCellSlots[i] : -1;
        if (recSlot >= 0 && recSlot < cellLen && cell[recSlot] == instance) {
            int32_t last = cellLen - 1;
            Instance* moved = (recSlot != last) ? cell[last] : nullptr;
            // A duplicate occurrence (moved == instance) was never observed, but fall
            // through to the linear sweep if it happens rather than miscounting.
            if (moved == nullptr || moved != instance) {
                if (moved != nullptr) {
                    cell[recSlot] = moved;
                    SpatialGrid_fixMovedSlot(moved, gridCoordinates, recSlot);
                }
                arrsetlen(cell, last);
                removedCount++;
                continue;
            }
        }
        // Stale slot (rare): linear sweep with swap-remove.
        g_gridEntriesScanned += cellLen; // TEMP
        for (int32_t j = 0; j < cellLen;) {
            if (cell[j] == instance) {
                int32_t last = cellLen - 1;
                if (j != last) {
                    cell[j] = cell[last];
                    SpatialGrid_fixMovedSlot(cell[j], gridCoordinates, j);
                }
                arrsetlen(cell, last);
                removedCount++;
                cellLen--;
            } else {
                j++;
            }
        }
    }

    // Retain the defensive full-grid recovery when non-empty cached tracking
    // data proves inconsistent. An empty list means the instance has not been
    // inserted (or had no valid collision box); room changes discard the old
    // grid after clearing persistent-instance tracking. Scanning for every new
    // untracked bullet would recreate the dense-pattern cost this fast path is
    // intended to avoid.
    if (trackedCellCount > 0 && removedCount < trackedCellCount) {
        g_gridFullScans++; // TEMP
        repeat(totalCells, cellIndex) {
            Instance** cell = grid->grid[cellIndex];
            int32_t cellLen = (int32_t) arrlen(cell);
            g_gridEntriesScanned += cellLen; // TEMP
            for (int32_t j = 0; j < cellLen;) {
                if (cell[j] == instance) {
                    // No record repair here: this path is rare, and any disturbed slot
                    // self-heals through the verified fast path / linear fallback.
                    cell[j] = cell[cellLen - 1];
                    arrsetlen(cell, cellLen - 1);
                    removedCount++;
                    cellLen--;
                } else {
                    j++;
                }
            }
        }
    }

    if (trackedCellCount > 0 || removedCount > 0) {
        arrsetlen(instance->collisionCells, 0);
        arrsetlen(instance->collisionCellSlots, 0); // Keep the parallel slot array in sync
        instance->spatialGridDirty = false;
    }
}

#ifdef THWWW_TEMP_COLLISION_STATS
static uint64_t tSyncCalls = 0, tSyncNonEmpty = 0, tSyncDirtyTotal = 0, tSyncNanos = 0, tSyncSkipped = 0, tSyncFull = 0;
static bool tSyncDumped = false;
#endif

void SpatialGrid_syncGrid(Runner* runner, SpatialGrid* grid) {
    bool requiresResync = arrlen(grid->dirtyInstances);
#ifdef THWWW_TEMP_COLLISION_STATS
    tSyncCalls++;
    uint64_t tSyncT0 = requiresResync ? nowNanos() : 0;
#endif
    if (!requiresResync) return;
#ifdef THWWW_TEMP_COLLISION_STATS
    tSyncNonEmpty++;
    tSyncDirtyTotal += (uint64_t) arrlen(grid->dirtyInstances);
#endif

#ifdef ENABLE_SPATIAL_GRID_LOGS
    logInfo("SpatialGrid: Syncing grid with %d dirty instances\n", arrlen(grid->dirtyInstances));
#endif

    repeat(arrlen(grid->dirtyInstances), i) {
        int32_t instanceId = grid->dirtyInstances[i];
        Instance* instance = hmget(runner->instancesById, instanceId);

        // We do not care about removed/inactive/destroyed instances, because they would've been already been removed from the grid on the "SpatialGrid_markInstanceAsDirty" call
        // We also do not care if the spatial grid is not dirty
        if (instance == nullptr || !instance->active || instance->destroyed || !instance->spatialGridDirty)
            continue;

        instance->spatialGridDirty = false;

        InstanceBBox bbox = Collision_getBBox(runner, instance);

        if (bbox.valid) {
            // Fast path: most movers stay inside the same cells frame to frame.
            // collisionCells stores packed coords in gx-outer/gy-inner insert order,
            // so an equal footprint means remove+reinsert would be a no-op: skip it.
            SpatialGridRange range = SpatialGrid_computeCellRange(grid, bbox.left, bbox.top, bbox.right, bbox.bottom);
            int32_t wantCells = (range.maxGridX - range.minGridX + 1) * (range.maxGridY - range.minGridY + 1);
            if (arrlen(instance->collisionCells) == wantCells) {
                int32_t ci = 0;
                bool same = true;
                for (int32_t gx = range.minGridX; range.maxGridX >= gx && same; gx++) {
                    for (int32_t gy = range.minGridY; range.maxGridY >= gy; gy++) {
                        if (instance->collisionCells[ci++] != (int32_t) SpatialGrid_packGridCoordinates((uint16_t) gx, (uint16_t) gy)) {
                            same = false;
                            break;
                        }
                    }
                }
                if (same) {
#ifdef THWWW_TEMP_COLLISION_STATS
                    tSyncSkipped++;
#endif
                    continue;
                }
            }
        }

#ifdef THWWW_TEMP_COLLISION_STATS
        tSyncFull++;
#endif
        // Remove from old cells
        SpatialGrid_removeInstance(grid, instance);

        arrsetlen(instance->collisionCells, 0);
        arrsetlen(instance->collisionCellSlots, 0); // Keep the parallel slot array in sync

        if (!bbox.valid)
            continue;

        SpatialGridRange range = SpatialGrid_computeCellRange(grid, bbox.left, bbox.top, bbox.right, bbox.bottom);

        for (int32_t gx = range.minGridX; range.maxGridX >= gx; gx++) {
            for (int32_t gy = range.minGridY; range.maxGridY >= gy; gy++) {
                int32_t insertCell = SpatialGrid_cellIndex(grid, gx, gy);
                arrput(grid->grid[insertCell], instance);
                arrput(instance->collisionCells, SpatialGrid_packGridCoordinates(gx, gy));
                arrput(instance->collisionCellSlots, (int32_t) arrlen(grid->grid[insertCell]) - 1); // Slot record for O(1) removal (parallel to collisionCells)
            }
        }

        // And that's it for now!
    }

    arrsetlen(grid->dirtyInstances, 0);
#ifdef THWWW_TEMP_COLLISION_STATS
    tSyncNanos += nowNanos() - tSyncT0;
    if (runner->frameCount >= 599 && !tSyncDumped) {
        tSyncDumped = true;
        logInfo("TEMPCOLL sync: calls=%llu nonempty=%llu dirtyTotal=%llu nanos=%llu (avgNs/nonempty=%.0f avgDirty/nonempty=%.1f)\n",
            (unsigned long long) tSyncCalls, (unsigned long long) tSyncNonEmpty, (unsigned long long) tSyncDirtyTotal,
            (unsigned long long) tSyncNanos, tSyncNonEmpty ? (double) tSyncNanos / (double) tSyncNonEmpty : 0.0,
            tSyncNonEmpty ? (double) tSyncDirtyTotal / (double) tSyncNonEmpty : 0.0);
        logInfo("TEMPCOLL sync: skipped=%llu full=%llu (skipRate=%.1f%%)\n",
            (unsigned long long) tSyncSkipped, (unsigned long long) tSyncFull,
            (tSyncSkipped + tSyncFull) ? 100.0 * (double) tSyncSkipped / (double)(tSyncSkipped + tSyncFull) : 0.0);
    }
#endif
}

void SpatialGrid_markInstanceAsDirty(SpatialGrid* grid, Instance* dirtyInstance) {
    // Any grid-affecting change also invalidates the memoized collision bbox.
    dirtyInstance->cachedBBox.valid = false;
    // Structs should NOT be included in the spatial grid!
    if (dirtyInstance->objectIndex == STRUCT_OBJECT_INDEX)
        return;

    if (!dirtyInstance->active || dirtyInstance->destroyed) {
        // Inactive/destroyed instances are removed immediately. Also clear a
        // pending dirty flag even when the instance had not reached its first
        // insertion yet; otherwise a sync can discard its queued ID while it is
        // inactive and a later reactivation would incorrectly skip re-queuing.
        SpatialGrid_removeInstance(grid, dirtyInstance);
        dirtyInstance->spatialGridDirty = false;
        return;
    }

    if (dirtyInstance->spatialGridDirty)
        return;

    dirtyInstance->spatialGridDirty = true;

    // You may be thinking "why don't we store the Instance pointer?"
    // Well, it is because the Instance* may not be valid when SpatialGrid_syncGrid is ran
    arrput(grid->dirtyInstances, dirtyInstance->instanceId);
}

SpatialGridQuery SpatialGrid_prepareQuery(Runner* runner, GMLReal x1, GMLReal y1, GMLReal x2, GMLReal y2, int32_t target) {
    // We do let INSTANCE_ALL through
    requireMessageFormatted(__FILE__, __LINE__, target >= 0 || target == INSTANCE_ALL, "SpatialGrid: [%s] Query target cannot be instance type %d!", runner->vmContext->currentCodeName, target);

    SpatialGridRange callerRange = SpatialGrid_computeCellRange(runner->spatialGrid, x1, y1, x2, y2);
    bool filterByObject = target >= 0 && INSTANCE_ID_BASE > target;
    bool filterByInstanceId = target >= INSTANCE_ID_BASE;
    uint32_t queryId = ++runner->collisionQueryCounter;
    SpatialGridQuery ret = {0};
    ret.range = callerRange;
    ret.filterByObject = filterByObject;
    ret.filterByInstanceId = filterByInstanceId;
    ret.matchAll = target == INSTANCE_ALL;
    ret.queryId = queryId;
    return ret;
}
