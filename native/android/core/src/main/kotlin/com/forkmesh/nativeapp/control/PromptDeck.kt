package com.forkmesh.nativeapp.control

class PromptDeck private constructor(val prompts: List<String>) {
    val isEmpty: Boolean get() = prompts.isEmpty()

    val count: Int get() = prompts.size

    fun clamped(index: Int): Int = maxOf(0, minOf(index, count - 1))

    fun prompt(at: Int): String = if (isEmpty) "" else prompts[clamped(at)]

    fun headline(at: Int): String = prompt(at).split(WHITESPACE).filter { it.isNotEmpty() }.joinToString(" ")

    fun older(than: Int): Int = clamped(than + 1)

    fun newer(than: Int): Int = clamped(than - 1)

    fun position(index: Int): String = if (count < 2) "" else "${count - clamped(index)}/$count"

    fun behind(index: Int): Int = minOf(2, maxOf(0, count - 1 - clamped(index)))

    override fun equals(other: Any?): Boolean = other is PromptDeck && other.prompts == prompts

    override fun hashCode(): Int = prompts.hashCode()

    companion object {
        private val WHITESPACE = Regex("\\s+")

        fun said(said: List<String>): PromptDeck = PromptDeck(said.asReversed().mapNotNull { text -> text.trim().ifEmpty { null } })

        fun of(blocks: List<TranscriptBlock>): PromptDeck = said(
            blocks.mapNotNull { block ->
                when (block) {
                    is TranscriptBlock.Prompt -> block.text
                    is TranscriptBlock.Steer -> if (block.fromYou) block.text else null
                    else -> null
                }
            },
        )
    }
}
