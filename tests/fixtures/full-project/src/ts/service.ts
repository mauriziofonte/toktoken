import { Config, DataItem } from './types';

interface CacheEntry<T> {
    data: T;
    expiresAt: number;
}

export class DataService {
    private config: Config;
    private store: Map<string, CacheEntry<DataItem[]>> = new Map();
    private ttlMs: number;

    constructor(config: Config) {
        this.config = config;
        this.ttlMs = (config.cacheTtl ?? 300) * 1000;
    }

    async fetch(endpoint: string): Promise<DataItem[]> {
        const cached = this.store.get(endpoint);

        if (cached && cached.expiresAt > Date.now()) {
            return cached.data;
        }

        const url = `${this.config.baseUrl}${endpoint}`;
        const response = await globalThis.fetch(url, {
            headers: {
                'Authorization': `Bearer ${this.config.apiKey}`,
                'Content-Type': 'application/json',
            },
        });

        if (!response.ok) {
            throw new Error(`Request failed: ${response.status} ${response.statusText}`);
        }

        const items: DataItem[] = await response.json();

        this.cache(endpoint, items);

        return items;
    }

    transform<R>(items: DataItem[], mapper: (item: DataItem) => R): R[] {
        return items
            .filter((item) => item.active)
            .sort((a, b) => b.priority - a.priority)
            .map(mapper);
    }

    cache(key: string, data: DataItem[]): void {
        this.store.set(key, {
            data,
            expiresAt: Date.now() + this.ttlMs,
        });

        if (this.store.size > (this.config.maxCacheSize ?? 1000)) {
            const oldest = this.store.keys().next().value;
            if (oldest !== undefined) {
                this.store.delete(oldest);
            }
        }
    }

    invalidate(key?: string): void {
        if (key) {
            this.store.delete(key);
        } else {
            this.store.clear();
        }
    }
}
