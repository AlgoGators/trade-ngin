# Risk module examples (schema 2)

Shapes, not values. Nothing here is loaded by any runner: the numbers are placeholders,
and a real book's numbers live in `config_template/portfolios/<book>/risk.json`. Each
`*.risk.json` is a complete risk.json that the loader accepts (or, where the name says so,
rejects with a named rule), so they double as worked examples of the rules.

| file | shows |
|---|---|
| `none.risk.json` | a book that runs NO risk layer, and the ruling that made it so |
| `constant_scale.risk.json` | a fixed cut, the smallest module there is |
| `warn.risk.json` | a condition that logs and changes nothing |
| `refuse_on_condition.risk.json` | a condition that stops the book trading this rebalance |
| `carver_plus_warn.risk.json` | two modules at one scope: the gate plus a warning |
| `sleeve_assignment.portfolio.json` | the `sleeve_risk_modules` block, in portfolio.json |
