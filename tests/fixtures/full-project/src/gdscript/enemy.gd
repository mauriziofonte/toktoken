class_name Enemy
extends CharacterBody2D

signal defeated(enemy_name: String)
signal health_changed(new_health: int)

enum State {
    IDLE,
    PATROL,
    CHASE,
    ATTACK,
    DEAD
}

const MAX_HP: int = 100
const ATTACK_RANGE: float = 50.0
const MOVE_SPEED: float = 120.0
const DETECTION_RANGE: float = 200.0

var current_health: int = MAX_HP
var current_state: State = State.IDLE
var patrol_points: Array[Vector2] = []
var current_patrol_index: int = 0
var target: Node2D = null

func _ready() -> void:
    current_health = MAX_HP
    current_state = State.IDLE
    _setup_patrol_route()
    add_to_group("enemies")

func _physics_process(delta: float) -> void:
    match current_state:
        State.IDLE:
            _check_for_player()
        State.PATROL:
            _move_to_patrol_point(delta)
        State.CHASE:
            _chase_target(delta)
        State.ATTACK:
            attack()
        State.DEAD:
            pass

func attack() -> void:
    if target == null or not is_instance_valid(target):
        current_state = State.IDLE
        return
    var distance = global_position.distance_to(target.global_position)
    if distance > ATTACK_RANGE:
        current_state = State.CHASE
        return
    if target.has_method("take_damage"):
        target.take_damage(15)

func take_damage(amount: int) -> void:
    current_health = max(0, current_health - amount)
    health_changed.emit(current_health)
    if current_health <= 0:
        _die()

func heal(amount: int) -> void:
    current_health = min(MAX_HP, current_health + amount)
    health_changed.emit(current_health)

func _die() -> void:
    current_state = State.DEAD
    defeated.emit(name)
    queue_free()

func _setup_patrol_route() -> void:
    patrol_points = [
        global_position + Vector2(100, 0),
        global_position + Vector2(100, 100),
        global_position + Vector2(0, 100),
        global_position
    ]

func _check_for_player() -> void:
    var players = get_tree().get_nodes_in_group("player")
    for player in players:
        if global_position.distance_to(player.global_position) < DETECTION_RANGE:
            target = player
            current_state = State.CHASE
            return

func _move_to_patrol_point(delta: float) -> void:
    if patrol_points.is_empty():
        return
    var target_point = patrol_points[current_patrol_index]
    var direction = global_position.direction_to(target_point)
    velocity = direction * MOVE_SPEED
    move_and_slide()
    if global_position.distance_to(target_point) < 5.0:
        current_patrol_index = (current_patrol_index + 1) % patrol_points.size()

func _chase_target(delta: float) -> void:
    if target == null or not is_instance_valid(target):
        current_state = State.IDLE
        return
    var direction = global_position.direction_to(target.global_position)
    velocity = direction * MOVE_SPEED * 1.5
    move_and_slide()
