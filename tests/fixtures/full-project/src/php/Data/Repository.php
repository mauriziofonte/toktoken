<?php

declare(strict_types=1);

namespace App\Data;

/**
 * In-memory data store simulating database operations.
 */
class Repository
{
    /** @var array<string, array<int, array<string, mixed>>> */
    private array $tables = [];

    /**
     * Query rows from a table, optionally filtered by conditions.
     *
     * @param string $table
     * @param array<string, mixed> $conditions
     * @return array<int, array<string, mixed>>
     */
    public function query(string $table, array $conditions): array
    {
        if (!isset($this->tables[$table])) {
            return [];
        }

        if (empty($conditions)) {
            return array_values($this->tables[$table]);
        }

        return array_values(array_filter(
            $this->tables[$table],
            function (array $row) use ($conditions): bool {
                foreach ($conditions as $key => $value) {
                    if (!isset($row[$key]) || $row[$key] !== $value) {
                        return false;
                    }
                }
                return true;
            }
        ));
    }

    /**
     * Persist a record into a table. Requires an 'id' field.
     *
     * @param string $table
     * @param array<string, mixed> $record
     */
    public function save(string $table, array $record): void
    {
        if (!isset($this->tables[$table])) {
            $this->tables[$table] = [];
        }

        $id = (int) ($record['id'] ?? count($this->tables[$table]) + 1);
        $this->tables[$table][$id] = $record;
    }

    /**
     * Remove matching records from a table.
     *
     * @param string $table
     * @param array<string, mixed> $conditions
     * @return bool True if at least one record was removed.
     */
    public function remove(string $table, array $conditions): bool
    {
        if (!isset($this->tables[$table])) {
            return false;
        }

        $initialCount = count($this->tables[$table]);

        $this->tables[$table] = array_filter(
            $this->tables[$table],
            function (array $row) use ($conditions): bool {
                foreach ($conditions as $key => $value) {
                    if (isset($row[$key]) && $row[$key] === $value) {
                        return false;
                    }
                }
                return true;
            }
        );

        return count($this->tables[$table]) < $initialCount;
    }
}
