import { DataService } from './service';

interface AppOptions {
    port: number;
    host: string;
    debug: boolean;
}

export class Application {
    private service: DataService;
    private running: boolean = false;
    private options: AppOptions;
    private shutdownHandlers: Array<() => Promise<void>> = [];

    constructor(service: DataService) {
        this.service = service;
        this.options = { port: 3000, host: '0.0.0.0', debug: false };
    }

    async start(): Promise<void> {
        if (this.running) {
            throw new Error('Application is already running');
        }

        await this.service.fetch('/health');

        this.running = true;

        console.log(
            `Server listening on ${this.options.host}:${this.options.port}`
        );

        process.on('SIGTERM', async () => {
            await this.stop();
        });
    }

    async stop(): Promise<void> {
        if (!this.running) {
            return;
        }

        for (const handler of this.shutdownHandlers) {
            try {
                await handler();
            } catch (err) {
                console.error('Shutdown handler failed:', err);
            }
        }

        this.running = false;
        console.log('Application stopped gracefully');
    }

    configure(options: Partial<AppOptions>): Application {
        this.options = { ...this.options, ...options };

        if (this.options.debug) {
            console.log('Debug mode enabled, configuration:', this.options);
        }

        return this;
    }

    onShutdown(handler: () => Promise<void>): void {
        this.shutdownHandlers.push(handler);
    }

    isRunning(): boolean {
        return this.running;
    }
}
