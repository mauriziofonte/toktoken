module Simulation

export simulate, step, Particle, GRAVITY

const GRAVITY = 9.81
const TIME_STEP = 0.01
const MAX_ITERATIONS = 10000

struct Particle
    x::Float64
    y::Float64
    vx::Float64
    vy::Float64
    mass::Float64
end

mutable struct SimulationState
    particles::Vector{Particle}
    time::Float64
    iteration::Int
end

function simulate(particles::Vector{Particle}, duration::Float64)::SimulationState
    state = SimulationState(copy(particles), 0.0, 0)
    n_steps = min(Int(ceil(duration / TIME_STEP)), MAX_ITERATIONS)

    for i in 1:n_steps
        state = step(state)
        if all(p -> abs(p.vy) < 1e-6 && p.y <= 0.0, state.particles)
            break
        end
    end

    return state
end

function step(state::SimulationState)::SimulationState
    new_particles = map(state.particles) do p
        ay = -GRAVITY
        new_vy = p.vy + ay * TIME_STEP
        new_vx = p.vx * 0.999
        new_x = p.x + new_vx * TIME_STEP
        new_y = p.y + new_vy * TIME_STEP

        if new_y < 0.0
            new_y = 0.0
            new_vy = -new_vy * 0.8
        end

        Particle(new_x, new_y, new_vx, new_vy, p.mass)
    end

    return SimulationState(new_particles, state.time + TIME_STEP, state.iteration + 1)
end

function kinetic_energy(p::Particle)::Float64
    return 0.5 * p.mass * (p.vx^2 + p.vy^2)
end

function potential_energy(p::Particle)::Float64
    return p.mass * GRAVITY * p.y
end

function total_energy(state::SimulationState)::Float64
    return sum(p -> kinetic_energy(p) + potential_energy(p), state.particles)
end

function create_particle(x::Float64, y::Float64;
                         vx::Float64=0.0, vy::Float64=0.0,
                         mass::Float64=1.0)::Particle
    return Particle(x, y, vx, vy, mass)
end

end # module
