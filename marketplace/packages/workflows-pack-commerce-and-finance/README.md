# Workflows: Commerce and finance

An integration pack for the [Workflows](../workflows/README.md) package. It adds these 8 steps to the Add step menu:

- Stripe: Customers, products and charges in Stripe.
- PayPal: Read PayPal orders and invoices.
- Shopify: Orders and products in a Shopify store.
- WooCommerce: Orders and products in a WooCommerce store.
- Square: Payments and customers in Square.
- Paddle: Transactions and customers in Paddle Billing.
- Lemon Squeezy: Orders and subscriptions in Lemon Squeezy.
- QuickBooks Online: Query and create QuickBooks records.

It depends on Workflows. Installing it installs Workflows too; removing Workflows removes this pack from every device on your account. Removing only this pack hides its integrations from the Add step menu; workflows that already use them keep running. The pack carries no code: it lists its integrations in a `workflows.integrations` contribution and the Workflows page filters its menu by it. See `docs/app/workflows.md`.
