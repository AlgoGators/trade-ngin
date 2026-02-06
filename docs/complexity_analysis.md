# Complexity Analysis & Bottlenecks

This document details the performance characteristics of the core trading components based on the original codebase.

## ⚡ Executive Summary: The Bottlenecks

The primary bottleneck in the system is the **Dynamic Optimizer**, specifically when re-calculating tracking error during its optimization loop.

| Component | Complexity | Bottleneck Status |
| :--- | :--- | :--- |
| **Dynamic Optimizer** | **$O(N^3)$** | 🔴 **CRITICAL** (Cubic scaling) |
| **Risk Manager** | **$O(T \cdot N^2)$** | 🟡 **MODERATE** (Quadratic scaling) |
| **Portfolio Manager** | **$O(N^3)$** | 🔴 **CRITICAL** (Driven by Optimizer) |

*   **$N$**: Number of Symbols (Assets)
*   **$T$**: History Length (Lookback Period)

---

## 1. Dynamic Optimizer (The Big Bottleneck)
**Complexity:** $O(I \cdot N^3)$  
*(where I = Iterations, N = Symbols)*

This component is the most computationally expensive part of the system.

### Why is it slow?
It uses a "Greedy Algorithm" that tries to add/remove 1 contract for *every single symbol* to see if it improves the portfolio.
1.  **The Outer Loop**: Runs for a fixed number of iterations ($I \approx 50$).
2.  **The Candidate Loop**: Iterates through every symbol ($N$) to propose a trade.
3.  **The Calculation (The Flaw)**: For *each* proposal, it fully re-calculates the Tracking Error taking **$O(N^2)$**.

$$ \text{Total Work} \approx \text{Iterations} \times N \times N^2 = O(N^3) $$

**Source Reference**: `src/optimization/dynamic_optimizer.cpp` -> `optimize_single_period` calling `calculate_tracking_error` inside loop inside loop.

---

## 2. Risk Manager
**Complexity:** $O(T \cdot N^2)$  
*(where T = History Days, N = Symbols)*

The Risk Manager is heavy but generally manageable because it usually runs once per update, unlike the optimizer which runs in a loop.

### Key Operations
1.  **Covariance Matrix Construction:** calculating how every asset correlates with every other asset requires iterating through history ($T$) for every pair ($N \times N$).
2.  **Value at Risk (VaR):** Sorting historical returns takes $O(T \log T)$.

**Source Reference**: `src/risk/risk_manager.cpp` -> `create_market_data` -> `covariance` calculation logic.

---

## 3. Portfolio Manager
**Complexity:** $O(N^3)$

The Portfolio Manager coordinates the other components. Its own logic is fast ($O(N)$), but because it calls the Dynamic Optimizer, it inherits that cubic complexity.

**Source Reference**: `src/portfolio/portfolio_manager.cpp` -> `optimize_positions` calling `optimizer_->optimize`.

---

## 🚀 Recommendations

To fix the critical $O(N^3)$ bottleneck in the Dynamic Optimizer:

**Optimization:** "Incremental Updates"
Instead of re-calculating the entire matrix tracking error ($O(N^2)$) for every single candidate trade, you can mathematically calculate the *change* in error in just **$O(N)$** time.

*   **Current:** Full Re-calc ($O(N^3)$ total)
*   **Proposed:** Delta Update ($O(N^2)$ total)

This change would make the optimizer **100x faster** for a 100-symbol portfolio.
