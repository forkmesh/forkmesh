# Workflows: Email

An integration pack for the [Workflows](../workflows/README.md) package. It adds these 8 steps to the Add step menu:

- SendGrid: Send email and manage marketing contacts.
- Mailgun: Send email through a Mailgun domain.
- Postmark: Send transactional email.
- Resend: Send email with Resend.
- Mailchimp: Add subscribers to Mailchimp audiences.
- Brevo: Send transactional email and manage contacts.
- Kit (ConvertKit): Add subscribers and tags in Kit.
- MailerLite: Create or update subscribers.

It depends on Workflows. Installing it installs Workflows too; removing Workflows removes this pack from every device on your account. Removing only this pack hides its integrations from the Add step menu; workflows that already use them keep running. The pack carries no code: it lists its integrations in a `workflows.integrations` contribution and the Workflows page filters its menu by it. See `docs/app/workflows.md`.
