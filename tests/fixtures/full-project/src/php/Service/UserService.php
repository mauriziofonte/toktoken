<?php

declare(strict_types=1);

namespace App\Service;

use App\Data\Repository;

/**
 * Domain service for user operations.
 */
class UserService
{
    private Repository $repository;
    private int $nextId = 1;

    public function __construct(Repository $repository)
    {
        $this->repository = $repository;
    }

    /**
     * @return array<int, array<string, mixed>>
     */
    public function findAll(): array
    {
        $rows = $this->repository->query('users', []);

        return array_map(fn(array $row): array => [
            'id'     => (int) $row['id'],
            'name'   => $row['name'],
            'email'  => $row['email'],
            'active' => (bool) $row['active'],
        ], $rows);
    }

    /**
     * @return array<string, mixed>|null
     */
    public function findById(int $id): ?array
    {
        $results = $this->repository->query('users', ['id' => $id]);

        if (count($results) === 0) {
            return null;
        }

        $row = $results[0];

        return [
            'id'     => (int) $row['id'],
            'name'   => $row['name'],
            'email'  => $row['email'],
            'active' => (bool) $row['active'],
        ];
    }

    /**
     * @param array<string, mixed> $data
     * @return array<string, mixed>
     */
    public function create(array $data): array
    {
        $record = [
            'id'         => $this->nextId++,
            'name'       => $data['name'],
            'email'      => $data['email'],
            'active'     => $data['active'] ?? true,
            'created_at' => date('Y-m-d H:i:s'),
        ];

        $this->repository->save('users', $record);

        return $record;
    }

    public function delete(int $id): bool
    {
        return $this->repository->remove('users', ['id' => $id]);
    }
}
