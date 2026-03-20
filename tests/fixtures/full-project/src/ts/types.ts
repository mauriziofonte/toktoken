export enum Status {
    Active = 'active',
    Inactive = 'inactive',
    Pending = 'pending',
    Archived = 'archived',
}

export interface Config {
    baseUrl: string;
    apiKey: string;
    cacheTtl?: number;
    maxCacheSize?: number;
    retryAttempts?: number;
    debug?: boolean;
}

export interface DataItem {
    id: string;
    name: string;
    status: Status;
    active: boolean;
    priority: number;
    metadata?: Record<string, unknown>;
    tags?: string[];
    createdAt: string;
    updatedAt?: string;
}

export type Result<T> = {
    success: true;
    data: T;
    count: number;
} | {
    success: false;
    error: string;
    code: number;
};

export type SortDirection = 'asc' | 'desc';

export type FilterPredicate<T> = (item: T) => boolean;
