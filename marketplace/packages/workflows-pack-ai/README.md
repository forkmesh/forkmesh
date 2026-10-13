# Workflows: AI

An integration pack for the [Workflows](../workflows/README.md) package. It adds these 9 steps to the Add step menu:

- OpenAI: Chat completions, embeddings and images with OpenAI.
- Anthropic Claude: Send a prompt to Claude with the Messages API.
- Mistral AI: Chat completions with Mistral models.
- Groq: Fast chat completions on Groq.
- Perplexity: Answers with web search from Perplexity.
- Google Gemini: Generate text with Gemini models.
- Cohere: Chat with Cohere Command models.
- Hugging Face: Run models on the Hugging Face Inference API.
- DeepL: Translate text with DeepL.

It depends on Workflows. Installing it installs Workflows too; removing Workflows removes this pack from every device on your account. Removing only this pack hides its integrations from the Add step menu; workflows that already use them keep running. The pack carries no code: it lists its integrations in a `workflows.integrations` contribution and the Workflows page filters its menu by it. See `docs/app/workflows.md`.
