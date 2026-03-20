import { authenticate } from './middleware';

function createRouter() {
    const routes = new Map();
    const middleware = [authenticate];

    function addRoute(method, path, handler) {
        const key = `${method.toUpperCase()}:${path}`;

        if (routes.has(key)) {
            throw new Error(`Route already registered: ${key}`);
        }

        routes.set(key, { method: method.toUpperCase(), path, handler });

        return { addRoute, handleRequest };
    }

    function handleRequest(req) {
        const key = `${req.method}:${req.path}`;
        const route = routes.get(key);

        if (!route) {
            return { status: 404, body: { error: 'Not Found', path: req.path } };
        }

        for (const mw of middleware) {
            const result = mw(req);

            if (result && result.status >= 400) {
                return result;
            }
        }

        try {
            const body = route.handler(req);
            return { status: 200, body };
        } catch (err) {
            return {
                status: 500,
                body: { error: 'Internal Server Error', message: err.message },
            };
        }
    }

    function listRoutes() {
        return Array.from(routes.values()).map((r) => ({
            method: r.method,
            path: r.path,
        }));
    }

    return { addRoute, handleRequest, listRoutes };
}

export { createRouter };
