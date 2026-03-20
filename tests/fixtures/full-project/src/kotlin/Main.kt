import data.Store

data class AppConfig(val name: String, val port: Int, val debug: Boolean = false)

fun main(args: Array<String>) {
    val config = initialize(args)
    println("Starting ${config.name} on port ${config.port}")
    val runner = AppRunner(config)
    runner.execute()
}

fun initialize(args: Array<String>): AppConfig {
    val name = args.getOrElse(0) { "default-app" }
    val port = args.getOrElse(1) { "8080" }.toIntOrNull() ?: 8080
    val debug = args.any { it == "--debug" }
    return AppConfig(name = name, port = port, debug = debug)
}

class AppRunner(private val config: AppConfig) {
    companion object {
        const val VERSION = "1.3.0"
        private var instanceCount = 0
    }

    init {
        instanceCount++
    }

    fun execute() {
        val store = Store<String>()
        when {
            config.debug -> println("[DEBUG] Runner v$VERSION instance #$instanceCount")
            config.port < 1024 -> println("[WARN] Privileged port ${config.port}")
            else -> println("[INFO] Runner started normally")
        }
        store.save("app.status", "running")
        val status = store.load("app.status") ?: "unknown"
        println("Store reports status: $status")
    }
}
