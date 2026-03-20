<?php

declare(strict_types=1);

namespace App\Http;

use App\Service\UserService;

/**
 * Handles HTTP requests for user resources.
 */
class Controller
{
    private UserService $userService;

    public function __construct(UserService $userService)
    {
        $this->userService = $userService;
    }

    /**
     * List all users with optional filtering.
     *
     * @param array<string, mixed> $query
     * @return array<string, mixed>
     */
    public function index(array $query = []): array
    {
        $users = $this->userService->findAll();

        if (isset($query['active'])) {
            $users = array_filter($users, fn(array $u): bool => $u['active'] === true);
        }

        if (isset($query['sort']) && $query['sort'] === 'name') {
            usort($users, fn(array $a, array $b): int => strcmp($a['name'], $b['name']));
        }

        $limit = min((int) ($query['limit'] ?? 50), 100);

        return [
            'data'  => array_slice(array_values($users), 0, $limit),
            'total' => count($users),
            'limit' => $limit,
        ];
    }

    /**
     * Retrieve a single user by ID.
     */
    public function show(int $id): array
    {
        $user = $this->userService->findById($id);

        if ($user === null) {
            return ['error' => 'not_found', 'message' => "User {$id} does not exist"];
        }

        return ['data' => $user];
    }

    /**
     * Create a new user after validation.
     *
     * @param array<string, mixed> $payload
     * @return array<string, mixed>
     */
    public function store(array $payload): array
    {
        $errors = [];

        if (empty($payload['name']) || !is_string($payload['name'])) {
            $errors[] = 'name is required and must be a string';
        }

        if (empty($payload['email']) || !filter_var($payload['email'], FILTER_VALIDATE_EMAIL)) {
            $errors[] = 'a valid email is required';
        }

        if (!empty($errors)) {
            return ['error' => 'validation', 'messages' => $errors];
        }

        $user = $this->userService->create([
            'name'   => trim($payload['name']),
            'email'  => strtolower(trim($payload['email'])),
            'active' => (bool) ($payload['active'] ?? true),
        ]);

        return ['data' => $user, 'status' => 'created'];
    }

    /**
     * Delete a user by ID.
     */
    public function destroy(int $id): array
    {
        $existing = $this->userService->findById($id);

        if ($existing === null) {
            return ['error' => 'not_found', 'message' => "User {$id} does not exist"];
        }

        $deleted = $this->userService->delete($id);

        return $deleted
            ? ['status' => 'deleted', 'id' => $id]
            : ['error' => 'internal', 'message' => 'Failed to delete user'];
    }
}
