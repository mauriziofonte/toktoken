package com.example.service;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

public class OrderService {
    private final Map<String, Order> orders;
    private final List<String> auditLog;

    public OrderService() {
        this.orders = new HashMap<>();
        this.auditLog = new ArrayList<>();
    }

    public Order processOrder(String orderId, double amount) {
        validateOrder(orderId, amount);
        Order order = new Order(orderId, amount, "PROCESSED");
        orders.put(orderId, order);
        auditLog.add("PROCESS:" + orderId + ":" + System.currentTimeMillis());
        return order;
    }

    public void validateOrder(String orderId, double amount) {
        if (orderId == null || orderId.trim().isEmpty()) {
            throw new IllegalArgumentException("Order ID must not be empty");
        }
        if (amount < 0) {
            throw new IllegalArgumentException("Amount must be non-negative, got: " + amount);
        }
        if (orders.containsKey(orderId)) {
            throw new IllegalStateException("Duplicate order: " + orderId);
        }
    }

    public boolean cancelOrder(String orderId) {
        Order order = orders.get(orderId);
        if (order == null) {
            return false;
        }
        orders.put(orderId, new Order(orderId, order.amount(), "CANCELLED"));
        auditLog.add("CANCEL:" + orderId + ":" + System.currentTimeMillis());
        return true;
    }

    public Order getOrder(String orderId) {
        Order order = orders.get(orderId);
        if (order == null) {
            throw new IllegalArgumentException("Order not found: " + orderId);
        }
        return order;
    }

    public record Order(String id, double amount, String status) {}
}
