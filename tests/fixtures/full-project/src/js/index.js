import { createRouter } from './router';

const DEFAULT_PORT = 8080;

function configureApp(options = {}) {
    const config = {
        port: options.port || DEFAULT_PORT,
        host: options.host || '0.0.0.0',
        cors: options.cors !== false,
        trustProxy: options.trustProxy || false,
    };

    const router = createRouter();

    router.addRoute('GET', '/health', (_req) => ({
        status: 'ok',
        uptime: process.uptime(),
        timestamp: new Date().toISOString(),
    }));

    router.addRoute('GET', '/version', (_req) => ({
        name: 'full-project',
        version: '1.0.0',
    }));

    return { config, router };
}

async function startServer(options = {}) {
    const { config, router } = configureApp(options);

    const server = {
        listening: false,
        address: null,
    };

    try {
        server.listening = true;
        server.address = `${config.host}:${config.port}`;

        console.log(`Server started on ${server.address}`);

        return {
            server,
            router,
            close: () => {
                server.listening = false;
                console.log('Server shut down');
            },
        };
    } catch (err) {
        console.error('Failed to start server:', err.message);
        throw err;
    }
}

export { startServer, configureApp };
