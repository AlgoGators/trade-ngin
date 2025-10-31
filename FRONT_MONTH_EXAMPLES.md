# Front Month Contract Calculation Examples

This document shows examples of how the front month contract is calculated for both **monthly/all-months** contracts and **quarterly** contracts, using the actual expiry logic.

## Test Date: October 31, 2025 (2025-10-31)

---

## 1. Monthly / All Months Contracts

These contracts expire every month. The front month advances **the day after** the expiry date for that specific month.

### Example 1: CL (Crude Oil) - "All Months"
- **Contract Months**: All Months
- **Expiry Rule**: Default = 3rd Friday of the month
- **October 2025**: 3rd Friday = October 17, 2025
- **Current Date**: October 31, 2025
- **Calculation**: October 31 > October 17 expiry → **Advanced to November**
- **Front Month**: **CLX5** (November 2025)
- **Month Code**: X = November

### Example 2: RB (RBOB Gasoline) - "All Months"
- **Contract Months**: All Months  
- **Expiry Rule**: Default = 3rd Friday of the month
- **October 2025**: 3rd Friday = October 17, 2025
- **Current Date**: October 31, 2025
- **Calculation**: October 31 > October 17 expiry → **Advanced to November**
- **Front Month**: **RBX5** (November 2025)
- **Month Code**: X = November

### Example 3: MXP/6M (Mexican Peso) - "13 consecutive months"
- **Contract Months**: 13 consecutive months
- **Expiry Rule**: 2 business days prior to 3rd Wednesday
- **October 2025**: 3rd Wednesday = October 15, 2025 → 2 business days prior = October 13, 2025
- **Current Date**: October 31, 2025
- **Calculation**: October 31 > October 13 expiry → **Advanced to November**
- **Front Month**: **MXPX5** or **6MX5** (November 2025)
- **Month Code**: X = November

### Example 4: Same contracts on October 15, 2025 (BEFORE expiry)
- **Current Date**: October 15, 2025
- **CL October expiry**: October 17, 2025
- **Calculation**: October 15 ≤ October 17 expiry → **Still October**
- **Front Month**: **CLV5** (October 2025)
- **Month Code**: V = October

---

## 2. Quarterly Contracts

These contracts only trade specific months (typically MAR, JUN, SEP, DEC). The front month advances **the day after** the expiry date for the next listed contract month.

### Example 1: NQ (E-mini Nasdaq-100) - "MAR, JUN, SEP, DEC"
- **Contract Months**: MAR (3), JUN (6), SEP (9), DEC (12)
- **Expiry Rule**: 3rd Friday of the month
- **Current Date**: October 31, 2025
- **Current Month**: October (10)
- **Step 1**: Find next listed month ≥ October → **DEC (December)**
- **Step 2**: Calculate December 2025 expiry = 3rd Friday = December 19, 2025
- **Step 3**: October 31 < December 19 expiry → **Still December**
- **Front Month**: **NQZ5** (December 2025)
- **Month Code**: Z = December

### Example 2: NQ on December 20, 2025 (AFTER December expiry)
- **Current Date**: December 20, 2025
- **Current Month**: December (12)
- **Step 1**: Find next listed month ≥ December → **DEC (December)**
- **Step 2**: Calculate December 2025 expiry = December 19, 2025
- **Step 3**: December 20 > December 19 expiry → **Advanced to next in cycle**
- **Step 4**: Next in cycle = **MAR (March 2026)**
- **Front Month**: **NQH6** (March 2026)
- **Month Code**: H = March, Year = 6 (2026)

### Example 3: YM (E-mini Dow Jones) - "MAR, JUN, SEP, DEC"
- **Contract Months**: MAR (3), JUN (6), SEP (9), DEC (12)
- **Expiry Rule**: 3rd Friday of the month
- **Current Date**: October 31, 2025
- **Current Month**: October (10)
- **Step 1**: Find next listed month ≥ October → **DEC (December)**
- **Step 2**: Calculate December 2025 expiry = December 19, 2025
- **Step 3**: October 31 < December 19 expiry → **Still December**
- **Front Month**: **YMZ5** (December 2025)

### Example 4: 6B (British Pound) - "MAR, JUN, SEP, DEC"
- **Contract Months**: MAR (3), JUN (6), SEP (9), DEC (12)
- **Expiry Rule**: 2 business days prior to 3rd Wednesday
- **Current Date**: October 31, 2025
- **Current Month**: October (10)
- **Step 1**: Find next listed month ≥ October → **DEC (December)**
- **Step 2**: Calculate December 2025 expiry:
  - 3rd Wednesday of December = December 17, 2025
  - 2 business days prior = December 15, 2025
- **Step 3**: October 31 < December 15 expiry → **Still December**
- **Front Month**: **6BZ5** (December 2025)

### Example 5: 6B on December 16, 2025 (AFTER December expiry)
- **Current Date**: December 16, 2025
- **Step 1**: Find next listed month ≥ December → **DEC (December)**
- **Step 2**: December 2025 expiry = December 15, 2025
- **Step 3**: December 16 > December 15 expiry → **Advanced to next in cycle**
- **Step 4**: Next in cycle = **MAR (March 2026)**
- **Front Month**: **6BH6** (March 2026)

---

## Key Differences

| Contract Type | How Front Month is Determined | When It Advances |
|---------------|------------------------------|------------------|
| **Monthly/All Months** | Uses current calendar month. Calculates expiry for current month. | The day **after** that month's expiry date |
| **Quarterly** | Finds next listed delivery month from contract cycle. Calculates expiry for that month. | The day **after** that specific contract's expiry date |

---

## Expiry Rules by Symbol

| Symbol | Expiry Rule | Example |
|--------|-------------|---------|
| **Equity Index** (NQ, YM, ES, MES, MNQ, MYM, RTY) | 3rd Friday | Oct 2025 = Oct 17, 2025 |
| **FX** (6B, 6E, 6J, 6M, 6N, 6S) | 2 business days prior to 3rd Wednesday | Oct 2025 = ~Oct 13, 2025 |
| **CAD** (6C) | 1 business day prior to 3rd Wednesday | Oct 2025 = ~Oct 14, 2025 |
| **Energy** (CL, RB, NG) | 3rd Friday (default) | Oct 2025 = Oct 17, 2025 |
| **Metals** (GC, PL, SI) | 3rd last business day of month | Oct 2025 = ~Oct 29, 2025 |
| **Ag** (ZC, ZW, ZM, ZL, ZS, ZR, KE) | Business day prior to 15th | Oct 2025 = Oct 14, 2025 |
| **Treasuries** (ZN, UB) | 7 business days prior to last business day | Complex |
| **Livestock** (HE, LE, GF) | Various | Various |

---

## Summary for October 31, 2025

### Monthly Contracts (after October expiry):
- **CL** → CLX5 (November)
- **RB** → RBX5 (November)  
- **6M/MXP** → MXPX5 (November)

### Quarterly Contracts (before December expiry):
- **NQ** → NQZ5 (December)
- **YM** → YMZ5 (December)
- **6B** → 6BZ5 (December)

