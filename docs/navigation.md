# Navigation

Navigation lets characters find their own way around a level: a guard walks its patrol around the
crates, an enemy chases the player through a doorway and up the stairs, a villager wanders to a random
spot in the square. You say *where*; the engine works out *how*.

This guide starts with what the engine builds from your level, then shows agents (the characters that
walk), obstacles (things that move and get in the way), and the questions scripts can ask. The last part
is for C programmers.

## The idea in one picture

Seen from above, the level is cut into a grid of small squares, 0.25 m wide by default. For each square
the engine works out where a character could *stand*: on the floor, on top of a crate, on a bridge deck.
Each such spot is a **cell**. Next, the engine links cells a character can walk between: neighbors whose floors differ by no more
than a step, with enough headroom between them. Finding a way is then finding a chain of linked cells,
which is a well-understood problem (A*, below).

```
  side view of one column of squares        seen from above, cells and links
                                             . . . . . . . .
     ====  bridge deck   <- cell 2            . . . .|. . . .     | wall: no links across
                                              . . . .|. . . .
     ----  floor         <- cell 1            . . . . . . . .     (doorway: links go through)
```

A column can hold several cells, one above the other: the ground under a bridge and the bridge's deck
are both walkable, and the grid keeps them apart. That is what "2.5D, multi-layer" means.

## Where the grid comes from

The grid is built from the scene's **static colliders**: colliders with no rigid body, or with a
static one. Floors, walls, ramps, stairs, rocks. Triggers don't count, and neither do moving bodies
(crates you can push, doors that swing). Those are obstacles, covered below.

Building takes these steps (the details are in `navigation/navigation.h`):

1. **Rasterize.** Every collider's triangles are cut along the grid's squares. Each piece becomes a
   solid vertical slab in its column. A slab's top is walkable when the surface faces up and isn't
   steeper than `max_slope`.
2. **Low obstacles.** A non-walkable top just above a walkable one (a curb, a rail lying on the floor)
   counts as walkable too, because a character steps onto it.
3. **Cells.** A walkable top with at least `agent_height` of free space above it becomes a cell.
4. **Links.** Neighboring cells are linked when their floors are at most `step_height` apart and the
   space they share is tall enough.
5. **Erosion.** Cells whose centers are closer than `agent_radius` to a wall or a drop are removed.
   What is left is where the *center* of a character can go without its body touching a wall. This
   is why the walkable area stops short of the walls when you draw it.
6. **Islands.** Cells connected by links form an island. Two cells on different islands can't reach
   each other at all (the top of a tall crate is usually its own island), and the engine can tell that
   instantly.

The engine builds the grid the first time something asks for it after a scene loads, so scenes that
never use navigation pay nothing. A 60 x 60 m scene builds in a few tens of milliseconds.

### Settings: the size of the character

The grid is built for one size of character, set in the scene's environment:

```json
"environment": {
  "navigation": {"agent_radius": 0.4, "agent_height": 1.8, "max_slope": 45, "step_height": 0.3,
                 "cell_size": 0.25, "cell_height": 0.1}
}
```

| Key | Default | Meaning |
|---|---|---|
| `agent_radius` | 0.4 | How far a character's center stays from walls and drops. |
| `agent_height` | 1.8 | The headroom it needs. A table 1 m high has no walkable floor under it. |
| `max_slope` | 45 | The steepest walkable surface, in degrees. |
| `step_height` | 0.3 | The tallest step it walks up or down between neighboring cells (below `agent_height`). |
| `cell_size` | 0.25 | The width of the grid's squares. Smaller is more exact, and slower to build and search. |
| `cell_height` | 0.1 | The height resolution of floors. |
| `bounds_min`, `bounds_max` | everything | Only build inside this box. |

The defaults match the default character controller (`wpe.add_character`), so anything the grid calls
walkable, a character controller can walk.

**Doorways** need to be about `2 * agent_radius + cell_size` wide (1.05 m with the defaults) to stay
open. Narrower ones close in the erosion step. If a door your characters should use shows as blocked,
widen it, or build with a smaller `agent_radius` or `cell_size`.

### Seeing it

```
wpe_runtime --project my_game --debug-nav
```

draws the walkable area's outline (walls, drops and the edges of the grid) and lines about a meter
apart across it, in a different color for each island. With agents walking, their way ahead shows in
yellow, and the cells obstacles close show in red. In C, set `WPE_EngineDebugDraw.navigation`.

Things to look for:

- **An area that should be walkable isn't.** Its ceiling is too low, it is steeper than `max_slope`, it
  is narrower than the agent, or its mesh's triangles face down. Floors must face up: their corners run
  counter-clockwise seen from above, as glTF and the renderer expect.
- **Two areas have different colors but should connect.** The step between them is taller than
  `step_height`, or the opening is too narrow.
- **The tops of crates and walls show up.** That is correct: a character fits there. They are separate
  islands, so nothing paths onto them.

## In the editor

Press **N** in the viewport (or View > Navigation) to see the grid while you build the level. It is rebuilt a
moment after you stop changing the scene, so you can drag a wall and watch the walkable area change. The
View menu shows how many cells and islands the grid has, and has **Test a path...**: click a start and a goal
in the viewport, and the path an agent would walk shows in magenta. The navigation settings are in the
**Environment** panel; `nav_agent` and `nav_obstacle` are added like any component (Inspector > Add
component). Agents only walk in play mode. Tools and AI assistants driving the editor get the same through the `nav_info`, `nav_rebuild` and
`nav_path` commands ([editor.md](editor.md#commands)).

## Agents

An **agent** is an entity that walks itself to a destination. Give an entity a `nav_agent` in the scene
file:

```json
{"id": 7, "name": "guard", "transform": {"position": [2, 0, -4]},
 "character": {},
 "nav_agent": {"speed": 3, "turn_speed": 540},
 "script": {"path": "scripts/guard.luau"}}
```

or add one from a script with `wpe.add_nav_agent(entity, {speed = 3})`. Then tell it where to go:

```lua
wpe.nav_move_to(self.entity, vector.create(10, 0, 5))   -- walk there
wpe.nav_follow(self.entity, player)                       -- walk after the player, again and again
wpe.nav_stop(self.entity)                                 -- stand still
```

Each fixed step the agent:

1. **Plans** a path when it gets a new destination, or every `repath_interval` while following
   someone.
2. **Steers** toward the next corner of its path, slowing down over the last `slow_distance` and
   sidestepping other agents (`avoid_others`).
3. **Moves.** With a character controller (`character` above), it moves through it, with gravity: it
   falls, slides along walls and pushes crates like a player does. Without one, it moves its transform
   along the floor of the grid. That suits simple characters that don't need physics.
4. **Turns** to face the way it walks, at `turn_speed` degrees per second (0: it never turns). Forward is
   the entity's −Z, as everywhere in the engine.

When it gets there, its script hears `on_nav_arrived`. If the destination can't be reached (it is on a
roof, on another island, or off the grid), the agent walks as near as it can get and its script hears
`on_nav_failed`.

```lua
local Guard = {}
Guard.properties = { a = vector.create(-5, 0, 0), b = vector.create(5, 0, 0) }

function Guard:on_start()
	self.to_b = true
	wpe.nav_move_to(self.entity, self.b)
end

function Guard:on_nav_arrived()      -- reached a patrol point: turn around
	self.to_b = not self.to_b
	wpe.nav_move_to(self.entity, if self.to_b then self.b else self.a)
end

function Guard:on_nav_failed()       -- sent somewhere unreachable: back to the patrol
	wpe.nav_move_to(self.entity, self.a)
end

return Guard
```

`wpe.nav_state(entity)` says what an agent is doing (`"idle"`, `"moving"`, `"arrived"`,
`"unreachable"`) and how far it still has to walk. `wpe.nav_path(entity)` lists the corners ahead.
`scripts/nav_guard.luau` in the sample project patrols the master test scene.

| `nav_agent` key | Default | Meaning |
|---|---|---|
| `speed` | 3.5 | Top speed, m/s. |
| `acceleration` | 20 | How quickly it reaches its speed (and stops), m/s². |
| `arrive_distance` | 0.2 | It has arrived this close to the destination. |
| `slow_distance` | 1 | It slows down over the last stretch this long. |
| `turn_speed` | 720 | Degrees per second it turns to face its way (0: never). |
| `repath_interval` | 0.5 | Following: seconds between new paths. |
| `avoid_others` | true | Sidestep other agents. |

In a multiplayer game, an agent moves only on the side that owns its entity. Everyone else sees it
through replication, like any other networked entity.

## Obstacles

The grid is built once, so it doesn't know about a door that closes or a cart that rolls into the
way. Make such things **obstacles**:

```json
{"id": 9, "name": "cart", "transform": {"position": [4, 0.5, 0]}, "nav_obstacle": {"size": [2, 1, 1]}}
```

(or `wpe.add_nav_obstacle(entity, {size = vector.create(2, 1, 1)})`). An obstacle is a box in the
entity's space, scaled and turned with it. Wherever it moves, the cells under it, plus the agent's
radius around it, close to paths. This is called **carving**. Agents whose way it now blocks plan again
at once. A box high enough to walk under closes nothing below it. With `"carve": false`, agents only
sidestep it.

An agent caught inside a closed area (the door shut on it) walks out the shortest way. Paths never go
*into* a closed area.

## Asking questions

Scripts can ask about the grid directly. These respect obstacles too:

```lua
local path, length = wpe.nav_find_path(from, to)        -- corners, or nil when there is no way
if wpe.nav_can_walk(enemy_pos, player_pos) then ... end   -- a straight, unobstructed walk
if wpe.nav_reachable(a, b) then ... end                   -- any way at all (instant)
local spot = wpe.nav_nearest(click_point)                 -- the nearest place to stand
local wander = wpe.nav_random_point(home, 8)              -- somewhere reachable within 8 m
```

`nav_find_path` takes options: `partial = true` returns the way to the reachable point nearest an
unreachable goal, `raw = true` keeps every cell, and `obstacles = false` ignores obstacles. The full
list is in [scripting.md](scripting.md#navigation).

## How paths are found (for the curious)

**A\*** searches the cells outward from the start. It always expands next the cell whose distance
so far, plus a guess of the distance left, is smallest. The guess is the straight-line distance over
the grid, which never overestimates, so the first time A* reaches the goal it has found the shortest
chain. It moves straight and diagonally. A diagonal step is only allowed when both ways around the
corner are open, so paths never cut a wall's corner.

The chain of cells zigzags along the grid, so it is then **pulled straight**. From each corner the
path goes on to the farthest point of the chain that can be walked to in a straight line, like a
string pulled tight around the corners. What remains is a handful of corner points.

**Flow fields** answer the question for everyone at once. One pass (Dijkstra's algorithm, A* without
the guess) from the goal computes every cell's walking distance and the next cell toward it. A hundred
zombies chasing one player then cost one pass, and each zombie just looks up its cell. They are
available from C (`wpe_nav_flow_*`).

## From C

| Header | What it has |
|---|---|
| `navigation/navigation.h` | `WPE_NavGeometry` (triangles, or a world's static colliders), `wpe_nav_grid_build`, cell queries, debug lines. |
| `navigation/nav_path.h` | Nearest points, reachability, straight lines; `WPE_NavQuery` and `wpe_nav_find_path`; `WPE_NavBlockers` (carved cells); flow fields. |
| `navigation/nav_agents.h` | `WPE_NavAgent` and `WPE_NavObstacle` components and the `WPE_NavSystem` that runs them. |

```c
WPE_NavGrid* grid = NULL;
wpe_nav_grid_build_from_world(ecs, assets, NULL /* default settings */, &grid);
WPE_NavQuery* query = NULL;
wpe_nav_query_create(grid, &query);
WPE_NavPath path;
if (wpe_nav_find_path(query, start, goal, NULL, &path) == WPE_RESULT_OK) {
    /* path.points[0 .. path.count - 1]: borrowed until the next search with this query. */
}
wpe_nav_query_destroy(query);
wpe_nav_grid_destroy(grid);
```

The engine owns the scene's grid (`wpe_engine_nav_grid`, rebuilt after `wpe_engine_invalidate_nav_grid`)
and its navigation system (`wpe_engine_navigation`). Grids are immutable, so queries on different threads
may run at once, each with its own `WPE_NavQuery`. Searches are deterministic: the same level and
the same request always give the same path.

## Limits

- Avoidance between agents is simple steering, fine for tens of agents in a crowd, not hundreds.
- Static level geometry changing at runtime needs a rebuild (`wpe_engine_invalidate_nav_grid`); for
  things that move, use obstacles.
- An obstacle's footprint is its box's X and Z axes projected onto the ground: exact for boxes turned
  about Y, approximate for tilted ones.
- Convex colliders are rasterized from their mesh's own triangles (exact when the mesh is convex).
