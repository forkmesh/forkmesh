package com.shop

import com.shop.model.Item

class Checkout(private val cart: Cart) {
    fun pay(limit: Int): Boolean {
        val total = cart.total()
        return when {
            total <= 0 -> false
            total > limit -> false
            else -> true
        }
    }
}

sealed class Result
