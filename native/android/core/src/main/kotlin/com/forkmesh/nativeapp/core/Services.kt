package com.forkmesh.nativeapp.core

import java.util.concurrent.ConcurrentHashMap

object Services {
    private val registry = ConcurrentHashMap<Class<*>, Any>()

    fun <T : Any> put(type: Class<T>, instance: T?): T? {
        if (instance == null) registry.remove(type) else registry[type] = instance
        return instance
    }

    inline fun <reified T : Any> put(instance: T): T {
        put(T::class.java, instance)
        return instance
    }

    fun <T : Any> get(type: Class<T>): T? = registry[type]?.let(type::cast)

    inline fun <reified T : Any> get(): T? = get(T::class.java)

    inline fun <reified T : Any> require(): T =
        get(T::class.java) ?: error("${T::class.java.simpleName} is not registered")

    fun clear() = registry.clear()
}
