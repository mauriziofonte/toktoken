package com.example;

import com.example.service.OrderService;
import java.util.logging.Logger;

public class Application {
    private static final Logger logger = Logger.getLogger(Application.class.getName());
    private static final String VERSION = "2.4.1";

    private final OrderService orderService;
    private final String environment;
    private volatile boolean running;

    public Application(String environment) {
        this.environment = environment;
        this.orderService = new OrderService();
        this.running = false;
    }

    public static void main(String[] args) {
        String env = args.length > 0 ? args[0] : "production";
        Application app = new Application(env);
        try {
            app.start();
            Runtime.getRuntime().addShutdownHook(new Thread(app::shutdown));
            logger.info("Application started in " + env + " mode");
        } catch (Exception e) {
            logger.severe("Failed to start application: " + e.getMessage());
            System.exit(1);
        }
    }

    public void start() {
        if (running) {
            throw new IllegalStateException("Application is already running");
        }
        running = true;
        orderService.processOrder("INIT-001", 0.0);
        System.out.println("Application v" + VERSION + " started [" + environment + "]");
    }

    public String getStatus() {
        return running ? "RUNNING:" + environment : "STOPPED";
    }

    public void shutdown() {
        logger.info("Shutting down application...");
        running = false;
        orderService.cancelOrder("SHUTDOWN");
        System.out.println("Application stopped gracefully");
    }
}
