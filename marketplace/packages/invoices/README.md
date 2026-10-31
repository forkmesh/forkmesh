# Invoicing

Invoicing is an optional ForkMesh organization plugin. Members create invoices
for their CRM contacts (or anyone else), email them with a private
view-and-pay link, and see when each one is opened, viewed and paid.

- Invoices, payments and history are encrypted and shared only with members of
  the organization.
- Everything can be managed on the web (laid out for phones too), in a native
  desktop page, and in the iPhone and Android apps.
- With the CRM installed, an invoice can be linked to a CRM contact; the
  contact card in the CRM lists that contact's invoices and what they owe.
- The email carries an open-tracking image and a link to the invoice page at
  `/invoice`. The sender is pinged the first time it is opened, viewed and its
  pay link clicked, and when an unpaid invoice passes its due date.
- The dashboard shows outstanding, overdue and recently paid totals, the open
  and view rate, the average time to pay, and recent activity.

Install and uninstall use the shared organization plugin lifecycle. Uninstall
offers a JSON export, deletes the organization's invoices and settings, stops
every emailed link from working, and reverses the package schema when the
final installation is removed. Developer notes are in
[docs/marketplace/invoices.md](../../../docs/marketplace/invoices.md).
