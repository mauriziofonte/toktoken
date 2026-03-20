local M = {}

M.TILE_SIZE = 32
M.MAX_ENTITIES = 256
M.GRAVITY = 800

local entities = {}
local score = 0
local game_over = false
local delta_accumulator = 0.0

function M.init(config)
    entities = {}
    score = 0
    game_over = false
    delta_accumulator = 0.0

    if config and config.seed then
        math.randomseed(config.seed)
    else
        math.randomseed(os.time())
    end

    local player = {
        x = config and config.start_x or 100,
        y = config and config.start_y or 100,
        width = M.TILE_SIZE,
        height = M.TILE_SIZE,
        vx = 0,
        vy = 0,
        health = 100,
        type = "player"
    }
    table.insert(entities, player)

    return player
end

function M.update(dt)
    if game_over then return end

    delta_accumulator = delta_accumulator + dt
    local fixed_step = 1.0 / 60.0

    while delta_accumulator >= fixed_step do
        for i, entity in ipairs(entities) do
            if entity.type ~= "static" then
                entity.vy = entity.vy + M.GRAVITY * fixed_step
                entity.x = entity.x + entity.vx * fixed_step
                entity.y = entity.y + entity.vy * fixed_step

                if entity.y + entity.height > 600 then
                    entity.y = 600 - entity.height
                    entity.vy = 0
                end
            end
        end

        check_collisions()
        delta_accumulator = delta_accumulator - fixed_step
    end
end

function M.draw(renderer)
    if not renderer then return end

    for _, entity in ipairs(entities) do
        local color = {1, 1, 1, 1}
        if entity.type == "player" then
            color = {0.2, 0.6, 1.0, 1.0}
        elseif entity.type == "enemy" then
            color = {1.0, 0.2, 0.2, 1.0}
        elseif entity.type == "collectible" then
            color = {1.0, 0.9, 0.1, 1.0}
        end
        renderer:draw_rect(entity.x, entity.y, entity.width, entity.height, color)
    end

    renderer:draw_text(10, 10, string.format("Score: %d", score))
end

function M.reset()
    M.init({start_x = 100, start_y = 100})
end

function M.add_entity(entity)
    if #entities >= M.MAX_ENTITIES then
        return nil
    end
    table.insert(entities, entity)
    return entity
end

function M.get_score()
    return score
end

local function check_collisions()
    local player = entities[1]
    if not player then return end

    for i = #entities, 2, -1 do
        local e = entities[i]
        if overlaps(player, e) then
            if e.type == "collectible" then
                score = score + (e.value or 10)
                table.remove(entities, i)
            elseif e.type == "enemy" then
                player.health = player.health - (e.damage or 10)
                if player.health <= 0 then
                    game_over = true
                end
            end
        end
    end
end

local function overlaps(a, b)
    return a.x < b.x + b.width
       and a.x + a.width > b.x
       and a.y < b.y + b.height
       and a.y + a.height > b.y
end

return M
