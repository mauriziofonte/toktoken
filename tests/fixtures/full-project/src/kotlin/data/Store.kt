package data

class Store<T> {
    private val storage: MutableMap<String, T> = mutableMapOf()
    private var operationCount: Int = 0

    fun save(key: String, value: T): Boolean {
        require(key.isNotBlank()) { "Key must not be blank" }
        storage[key] = value
        operationCount++
        return true
    }

    fun load(key: String): T? {
        operationCount++
        return storage[key]
    }

    fun delete(key: String): T? {
        val previous = storage.remove(key)
        if (previous != null) {
            operationCount++
        }
        return previous
    }

    fun exists(key: String): Boolean = storage.containsKey(key)

    fun keys(): Set<String> = storage.keys.toSet()

    fun size(): Int = storage.size

    fun clear() {
        storage.clear()
        operationCount = 0
    }

    fun stats(): Map<String, Any> = mapOf(
        "entries" to storage.size,
        "operations" to operationCount
    )
}
