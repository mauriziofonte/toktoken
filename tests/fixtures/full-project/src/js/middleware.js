const rateLimitState = new Map();

function authenticate(req) {
    const authHeader = req.headers && req.headers['authorization'];

    if (!authHeader) {
        return { status: 401, body: { error: 'Missing Authorization header' } };
    }

    const parts = authHeader.split(' ');

    if (parts.length !== 2 || parts[0] !== 'Bearer') {
        return { status: 401, body: { error: 'Invalid Authorization format' } };
    }

    const token = parts[1];

    if (token.length < 32) {
        return { status: 403, body: { error: 'Invalid token' } };
    }

    req.userId = token.substring(0, 8);

    return null;
}

function rateLimit(windowMs = 60000, maxRequests = 100) {
    return function rateLimiter(req) {
        const clientId = req.userId || req.ip || 'anonymous';
        const now = Date.now();
        const record = rateLimitState.get(clientId);

        if (!record || now - record.windowStart > windowMs) {
            rateLimitState.set(clientId, { windowStart: now, count: 1 });
            return null;
        }

        record.count += 1;

        if (record.count > maxRequests) {
            return {
                status: 429,
                body: { error: 'Too Many Requests', retryAfter: Math.ceil((record.windowStart + windowMs - now) / 1000) },
            };
        }

        return null;
    };
}

function cors(allowedOrigins = ['*']) {
    return function corsHandler(req) {
        const origin = req.headers && req.headers['origin'];

        if (!origin) {
            return null;
        }

        const allowed = allowedOrigins.includes('*') || allowedOrigins.includes(origin);

        if (!allowed) {
            return { status: 403, body: { error: 'Origin not allowed' } };
        }

        req.corsHeaders = {
            'Access-Control-Allow-Origin': origin,
            'Access-Control-Allow-Methods': 'GET, POST, PUT, DELETE',
            'Access-Control-Allow-Headers': 'Content-Type, Authorization',
        };

        return null;
    };
}

export { authenticate, rateLimit, cors };
