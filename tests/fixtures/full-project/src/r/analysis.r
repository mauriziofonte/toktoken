library(stats)

SIGNIFICANCE_LEVEL <- 0.05
MIN_SAMPLE_SIZE <- 30

analyze_data <- function(data, target_col, feature_cols = NULL) {
    if (!is.data.frame(data)) {
        stop("Input must be a data frame")
    }
    if (nrow(data) < MIN_SAMPLE_SIZE) {
        warning("Sample size below minimum threshold")
    }

    if (is.null(feature_cols)) {
        feature_cols <- setdiff(names(data), target_col)
    }

    results <- list(
        n = nrow(data),
        target = target_col,
        features = feature_cols,
        summary = summary(data[[target_col]]),
        correlations = sapply(feature_cols, function(col) {
            if (is.numeric(data[[col]])) {
                cor(data[[target_col]], data[[col]], use = "complete.obs")
            } else {
                NA
            }
        })
    )

    return(results)
}

fit_model <- function(data, formula_str, method = "lm") {
    formula_obj <- as.formula(formula_str)

    model <- switch(method,
        "lm" = lm(formula_obj, data = data),
        "glm" = glm(formula_obj, data = data, family = binomial()),
        stop(paste("Unknown method:", method))
    )

    coefficients <- coef(model)
    r_squared <- if (method == "lm") summary(model)$r.squared else NULL
    residuals <- residuals(model)
    predictions <- fitted(model)

    result <- list(
        model = model,
        coefficients = coefficients,
        r_squared = r_squared,
        rmse = sqrt(mean(residuals^2)),
        predictions = predictions,
        method = method
    )

    return(result)
}

plot_results <- function(model_result, output_file = NULL) {
    if (!is.null(output_file)) {
        png(output_file, width = 800, height = 600)
    }

    par(mfrow = c(2, 2))

    plot(model_result$predictions, residuals(model_result$model),
         main = "Residuals vs Fitted",
         xlab = "Fitted Values", ylab = "Residuals",
         pch = 16, col = rgb(0.2, 0.4, 0.8, 0.6))
    abline(h = 0, col = "red", lty = 2)

    qqnorm(residuals(model_result$model), main = "Normal Q-Q Plot", pch = 16)
    qqline(residuals(model_result$model), col = "red")

    hist(residuals(model_result$model), breaks = 30,
         main = "Residual Distribution",
         xlab = "Residuals", col = "steelblue", border = "white")

    if (!is.null(model_result$r_squared)) {
        plot.new()
        text(0.5, 0.5, paste("R-squared:", round(model_result$r_squared, 4)),
             cex = 2, font = 2)
    }

    par(mfrow = c(1, 1))

    if (!is.null(output_file)) {
        dev.off()
    }
}

validate_data <- function(data, required_cols, numeric_cols = NULL) {
    missing <- setdiff(required_cols, names(data))
    if (length(missing) > 0) {
        stop(paste("Missing columns:", paste(missing, collapse = ", ")))
    }

    if (!is.null(numeric_cols)) {
        for (col in numeric_cols) {
            if (!is.numeric(data[[col]])) {
                data[[col]] <- as.numeric(data[[col]])
            }
        }
    }

    complete_rows <- complete.cases(data[, required_cols])
    if (sum(!complete_rows) > 0) {
        warning(paste("Removed", sum(!complete_rows), "rows with missing values"))
        data <- data[complete_rows, ]
    }

    return(data)
}
