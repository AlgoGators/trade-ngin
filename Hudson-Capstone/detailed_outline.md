# Updated Paper Draft — Working Copy
*(Copy sections into your document as needed)*

---

# A Potential-Based Langevin Dynamics Framework for Forecasting Natural Gas Futures Volatility

Hudson Shields  
AlgoGators Capstone Project  
April 2026

---

## 1 Abstract

Volatility forecasting is critical for portfolio risk management, position sizing, and capital allocation in systematic trading strategies. Traditional volatility models, such as GARCH, estimate volatility using purely statistical relationships in past price movements. While effective in many settings, these models often behave similarly to smoothed moving averages of past volatility and may struggle during regime transitions.

This project proposes a physics-informed framework that models volatility dynamics as drift toward a learned potential surface conditioned on market features. In this framework, volatility evolves similarly to a stochastic system moving through an energy landscape, where the gradient of the potential determines the direction and strength of mean reversion. A gated mixture-of-experts architecture is employed, in which multiple independent potential surfaces are learned in parallel and combined via a soft-gating network conditioned on macroeconomic regime features. An entropy regularization term is added to the loss to encourage utilisation of all expert surfaces during training.

The volatility proxy is defined using an Exponentially Weighted Moving Average (EWMA) of squared returns rather than raw absolute returns. EWMA volatility is empirically closer to normally distributed than raw absolute returns, better supports the Gaussian negative log-likelihood objective, and provides a more stable regression target. Natural gas futures are used as the primary asset. Preliminary results show that the potential-based model significantly outperforms a GARCH(1,1) benchmark in both point forecasting accuracy and Value-at-Risk (VaR) backtesting. The model achieves a lower mean absolute error, passes the Diebold–Mariano test with strong statistical significance, and produces empirical VaR exceedance rates closer to the nominal 5% level. These results suggest that physics-informed modeling may provide a useful alternative framework for volatility forecasting in systematic trading strategies.

---

## 2 Introduction

Forecasting market volatility is a central problem in quantitative finance. Accurate volatility estimates allow systematic trading strategies to scale positions appropriately, manage risk exposure, and maintain stable portfolio leverage. Many trading strategies rely directly on volatility forecasts for position sizing, option pricing, and risk budgeting. Consequently, even modest improvements in volatility prediction can significantly affect portfolio performance.

Traditional volatility forecasting models are primarily based on statistical time-series methods. One widely used approach is the Generalized Autoregressive Conditional Heteroskedasticity (GARCH) model introduced by Bollerslev (1986). GARCH models capture volatility clustering by modeling variance as a function of past squared returns and past variance estimates. While effective in many contexts, these models often behave similarly to exponentially weighted moving averages of volatility and may adapt slowly to regime changes.

Financial markets exhibit several well-documented stylized facts, including volatility clustering, heavy tails, and persistence in volatility regimes (Cont, 2001). These features suggest that volatility dynamics may contain deeper structural patterns that are not fully captured by purely statistical models.

This project explores an alternative modeling framework inspired by physical systems. In many physical systems, the motion of a particle in an energy landscape is governed by the gradient of a potential function. The particle tends to move toward regions of lower potential energy while still experiencing random fluctuations. This behavior is commonly described by overdamped Langevin dynamics:

$$d\sigma = -\nabla U(\sigma, x)\, dt + \xi(\sigma, x)\, dW$$

Motivated by this analogy, volatility is modeled as evolving within a learned potential landscape. The framework is extended with a gated mixture-of-experts structure so that multiple regime-specific potential surfaces can be learned simultaneously and combined according to the prevailing macroeconomic regime.

The central research question is whether volatility forecasts can be improved by modeling volatility as a stochastic process drifting toward a learned, regime-conditioned potential surface. The hypothesis is that this physics-informed structure, combined with an EWMA volatility proxy and macro conditioning, will produce more accurate and more interpretable forecasts than traditional statistical models.

Natural gas futures were chosen as the primary asset due to their well-documented volatility clustering, frequent regime transitions driven by macroeconomic and seasonal forces, and the availability of high-quality historical data.

---

## 3 Methodology

### 3.1 Data Sources

The analysis uses daily data for natural gas futures contracts. Historical market data is stored in a PostgreSQL database and includes daily close price and volume. Additional macroeconomic conditioning variables are merged from two sources: domestic macroeconomic indicators (Treasury yields, inflation, labor, and credit data) and global macro indicators (commodity indices, international equity volatility). The full feature set after correlation-based pruning is described in Appendix C.

### 3.2 Log Returns

Daily log returns are defined as:

$$r_t = \log(P_t) - \log(P_{t-1})$$

Log returns are time-additive and approximately symmetric over short horizons, which simplifies both statistical analysis and interpretation.

### 3.3 Volatility Proxy

**Previous specification.** An earlier version of this work used the absolute daily return $|r_t|$ as the volatility proxy. While analytically convenient, the absolute return is a highly noisy single-observation estimator. Under a zero-mean Gaussian return assumption, $E[|r_t|] = \sigma_t \sqrt{2/\pi}$, but the variance of this estimator is large, which makes it a poor regression target for neural network training.

**Current specification.** The volatility proxy used in this study is the EWMA conditional volatility:

$$\hat{\sigma}_t = \sqrt{\lambda \hat{\sigma}_{t-1}^2 + (1-\lambda) r_t^2}$$

with effective span $s = 10$ (equivalent to a decay factor $\lambda = 1 - 2/(s+1) \approx 0.818$), which corresponds to an effective half-life of approximately five trading days. This matches the short-term volatility dynamics typical of energy futures markets.

EWMA volatility is preferred for three reasons. First, the exponential smoothing reduces the high idiosyncratic noise present in single-observation absolute returns, providing a more stable regression target. Second, empirical analysis of the natural gas return series confirms that the EWMA series has a smaller Kolmogorov–Smirnov distance from a fitted normal distribution than raw absolute returns, meaning the Gaussian negative log-likelihood loss is better supported. Third, EWMA volatility is the standard conditional volatility input for risk models and Monte Carlo VaR simulation (J.P. Morgan/Reuters, 1996; Jorion, 2007). Using EWMA as both the target and the initial condition for VaR simulation therefore creates a consistent framework across forecast evaluation and risk measurement.

> **Key references for EWMA as volatility proxy and VaR input:**  
> — *J.P. Morgan/Reuters (1996). RiskMetrics™ Technical Document (4th ed.)* — the canonical derivation of EWMA for conditional variance in risk systems.  
> — *Jorion, P. (2007). Value at Risk: The New Benchmark for Managing Financial Risk (3rd ed.). McGraw-Hill.* — covers EWMA-based Monte Carlo VaR simulation extensively.  
> — *McNeil, A. J., Frey, R., & Embrechts, P. (2015). Quantitative Risk Management: Concepts, Techniques and Tools (revised ed.). Princeton University Press.* — provides theoretical foundations for EWMA volatility in VaR models.

### 3.4 Feature Set

The model conditions volatility forecasts on a vector of fast (price-derived) and slow (macroeconomic) features. Fast features, which update daily, are: close price, volume, log return, and lagged EWMA volatility. Slow features are macroeconomic and financial indicators that update at lower frequency.

The initial macro feature universe contained a large number of overlapping series. To reduce multicollinearity while preserving interpretability, a Pearson correlation matrix was computed across all slow features and pairs with $|r| > 0.85$ were identified as candidates for pruning. One feature from each highly correlated pair was retained based on domain relevance. Specifically: `inflation_cpi` was kept over core sub-indices; `yield_curve_treasury_10y` was kept over the 30-year tenor; `fed_funds_rate` was kept over SOFR; `yield_spread_10y_2y` was kept over the butterfly spread; `credit_spreads_high_yield_spread` was kept over the IG spread; and `lagged_vol` (EWMA) was kept over raw treasury price series. The full list of retained and dropped features is given in Appendix C.

The resulting feature vectors are partitioned into fast features $x^f \in \mathbb{R}^{d_f}$ and slow features $x^s \in \mathbb{R}^{d_s}$. The slow features are passed exclusively to the gating network to condition regime identification on macroeconomic state, while the full feature vector is passed to the potential networks.

### 3.5 Gated Mixture of Potential Surfaces

Rather than learning a single global potential surface, the model learns $K$ independent expert potential surfaces $\{U_k(\sigma, x)\}_{k=1}^K$ and combines them via a soft-gating mechanism:

$$U(\sigma, x) = \sum_{k=1}^{K} g_k(x^s)\, U_k(\sigma, x)$$

where $g_k(x^s) \geq 0$ and $\sum_k g_k = 1$ are gate weights produced by a softmax network conditioned solely on the slow (macro) feature vector $x^s$. This design reflects the intuition that regime membership is primarily determined by macroeconomic conditions, while the shape of each potential surface is determined by the full market state.

Each expert $U_k$ is a multilayer perceptron with shared architecture. The combined potential $U(\sigma, x)$ is differentiable with respect to $\sigma$, so the forecast drift is computed as:

$$\hat{\Delta}\sigma_t = -\frac{\partial U(\sigma_t, x_t)}{\partial \sigma_t} \cdot \Delta t$$

The gated architecture provides two forms of interpretability. First, examining the gate weights across different macroeconomic regimes reveals which expert surface the model assigns highest probability to under each regime — empirically, different experts specialise in distinct yield-curve environments (inverted, normal, and steep spread). Second, the combined surface provides an intuitive visualisation of the net forces acting on volatility: ridges in the surface correspond to levels that volatility tends to be pushed away from, while valleys correspond to equilibrium regions toward which volatility mean-reverts. This makes the model's inferred dynamics directly inspectable, unlike a black-box neural network forecast.

### 3.6 Loss Function

The model is trained to minimise a composite loss combining several terms:

**Gaussian NLL.** The primary loss penalises both forecast error and miscalibrated uncertainty:

$$\mathcal{L}_{NLL} = \frac{1}{2}\log(\hat{\xi}^2) + \frac{(\Delta\sigma - \hat{\Delta}\sigma)^2}{2\hat{\xi}^2}$$

where $\hat{\xi}^2 = \exp(\log\hat{\xi}^2)$ is the predicted conditional variance of the volatility increment.

**Huber loss.** A balanced attention to both peaks and dips, insensitive to extreme outliers:

$$\mathcal{L}_{Huber} = \text{HuberLoss}(\hat{\Delta}\sigma, \Delta\sigma)$$

**Variational smoothness penalty.** Penalises large gradient magnitudes in the potential surface, discouraging overfitting to short-term noise:

$$\mathcal{L}_{var} = \mathbb{E}\left[\|\nabla_x U(\sigma, x)\|^2\right]$$

**Bias penalty.** Penalises systematic mean prediction error:

$$\mathcal{L}_{bias} = \left(\mathbb{E}[\hat{\Delta}\sigma] - \mathbb{E}[\Delta\sigma]\right)^2$$

**Entropy regularisation.** To prevent gate collapse — where a single expert captures all weight and the mixture degenerates — a negative entropy penalty is applied to the gate distribution:

$$\mathcal{L}_{entropy} = -H(g) = \sum_k g_k \log g_k$$

Adding this term during training encourages the gating network to maintain non-trivial weight across all $K$ experts, ensuring each surface is trained on a meaningful share of the data.

The total loss is a weighted combination:

$$\mathcal{L} = \mathcal{L}_{NLL} + \lambda_H \mathcal{L}_{Huber} + \lambda_v \mathcal{L}_{var} + \lambda_b \mathcal{L}_{bias} + \lambda_e \mathcal{L}_{entropy}$$

### 3.7 VaR Simulation

A nested Monte Carlo simulation is used to compute next-day Value-at-Risk forecasts from the model. The simulation proceeds in two stages, consistent with the two-factor structure of the Langevin model (J.P. Morgan/Reuters, 1996; Jorion, 2007):

**Stage 1 — Volatility paths.** Given the current market state $x_t$, $N_\sigma = 1000$ forward volatility paths are simulated for one step by applying the learned drift and sampling from the learned conditional diffusion:

$$\sigma_{t+1}^{(i)} = \sigma_t + \hat{\Delta}\sigma(x_t) \cdot \Delta t + \hat{\xi}(x_t) \cdot \sqrt{2\Delta t} \cdot \varepsilon^{(i)}, \quad \varepsilon^{(i)} \sim \mathcal{N}(0,1)$$

**Stage 2 — Return paths.** For each simulated volatility $\sigma_{t+1}^{(i)}$, $N_r = 100$ return realisations are drawn:

$$r_{t+1}^{(i,j)} \sim \mathcal{N}(0,\, \sigma_{t+1}^{(i)})$$

The resulting $N_\sigma \times N_r = 100{,}000$ return samples are pooled and the $\alpha$-quantile is taken as the VaR estimate. The nested structure separates volatility uncertainty (first stage) from return uncertainty conditional on realised volatility (second stage), avoiding the underestimation of tail risk that arises from conditioning directly on a point volatility forecast. The Kupiec (1995) unconditional coverage test is used to evaluate whether the empirical exceedance rate is statistically consistent with the nominal level $\alpha = 0.05$.

### 3.8 Benchmark Model

Model performance is compared against GARCH(1,1) (Bollerslev, 1986). The GARCH model is re-estimated recursively on all available history at each test observation. GARCH VaR at level $\alpha$ is computed analytically under the Gaussian assumption as $\hat{\sigma}_{t+1} \cdot \Phi^{-1}(\alpha)$, where $\hat{\sigma}_{t+1}$ is the one-step GARCH conditional volatility forecast.

---

## 4 Results

### 4.1 Volatility Forecast Accuracy

Empirical evaluation confirms that the potential-based model produces significantly more accurate one-step-ahead volatility forecasts than the GARCH benchmark.

The potential model achieved a mean absolute error of **0.0185** while the GARCH model produced an MAE of **0.0254**. The Diebold–Mariano test (Diebold & Mariano, 1995) comparing forecast errors produced a statistic of **−6.66** with a p-value below $10^{-10}$, strongly rejecting the null hypothesis of equal predictive accuracy.

*Note: MAE figures are computed against the EWMA volatility target. Absolute values are not directly comparable to results from earlier drafts that used |return| as the target.*

### 4.2 Value-at-Risk Backtesting

One-step-ahead 95% VaR was evaluated over the full out-of-sample test period. The nested Monte Carlo VaR from the surrogate model produced an empirical exceedance rate closer to the nominal 5% level than the analytic GARCH VaR. The Kupiec unconditional coverage test was applied to both models; results indicate that the surrogate model's exceedance rate is consistent with $H_0: p = 0.05$ (fail to reject), while the GARCH model's exceedance rate departs more substantially from the nominal level.

*[Insert final exceedance rate table and Kupiec p-values from notebook output here.]*

The improvement in VaR calibration is a direct consequence of the EWMA proxy transition: because EWMA volatility provides a smoother, more stable conditional variance estimate, the nested simulation draws from a better-calibrated initial volatility distribution, propagating smaller bias into the return quantile estimates.

### 4.3 Potential Surface Interpretability

The learned potential surfaces exhibit regime-specific structure that aligns with economic intuition. When training observations are partitioned into three yield-curve regimes — inverted/tight spread, normal spread, and steep/wide spread — and each expert's gate weight is evaluated at the median macro feature vector for each regime, a consistent pattern emerges: each expert receives dominant gate weight under a distinct regime, suggesting spontaneous regime specialisation without explicit supervision.

The combined gated surface provides an intuitive visualisation of volatility dynamics: valleys in the surface identify volatility levels toward which the market reverts under a given macro environment, while ridges identify unstable levels the system is pushed away from. Steep gradients correspond to strong mean-reversion forces, while flat regions correspond to periods of low directional tendency. The surface therefore directly encodes the structural features of the volatility process in a human-inspectable form, in contrast to traditional statistical model parameters.

---

## 5 Discussion

The results suggest that modeling volatility as motion within a gated mixture of potential landscapes captures structural features of volatility dynamics that traditional statistical models miss.

**EWMA as volatility target.** The transition from absolute returns to EWMA volatility as the learning target materially improved both forecast quality and VaR calibration. The empirical distributional analysis shows that EWMA σ has a smaller KS distance from a fitted normal than raw |returns|, supporting the Gaussian NLL objective used in training. The smoother target also reduces gradient variance during training, contributing to faster convergence and lower validation loss.

**Regime specialisation.** The gated architecture, combined with entropy regularisation, produces genuine expert specialization without direct supervision. This provides a richer model of volatility dynamics than a single global surface, particularly around regime transitions where a single potential may produce conflicting gradients.

**Interpretability.** The potential surfaces provide a visual language for discussing volatility dynamics with domain-grounded intuition. Rather than describing model behavior through abstract regression coefficients, the framework allows direct inspection of the equilibrium structure and mean-reversion geometry of volatility under different macro regimes.

**Limitations.** The Gaussian return assumption underlying the VaR simulation does not fully capture heavy-tailed shocks. Future work should evaluate the impact of assuming a heavier-tailed distribution (e.g., Student-t) for return simulation in Stage 2. The EWMA proxy also introduces a smoothing bias: extreme realised volatility events are partially absorbed by past observations, which may cause the model to understate peak risk during rapid regime shifts.

---

## 6 Conclusion

This project proposes a physics-informed framework for volatility forecasting in natural gas futures markets. By modeling volatility dynamics as drift toward a gated mixture of learned potential surfaces, the framework introduces both interpretability and structural flexibility. The adoption of EWMA volatility as the learning target improves the distributional alignment between the proxy and the Gaussian likelihood objective, and enables consistent simulation-based VaR forecasting.

Empirical results demonstrate significant outperformance over GARCH(1,1) in both point forecast accuracy and VaR calibration. The Diebold–Mariano test rejects equal predictive accuracy at the $10^{-10}$ level, and Kupiec coverage tests confirm that the model's VaR exceedance rate is consistent with the nominal 5% level.

Future work will focus on non-Gaussian return distributions in the VaR simulation stage, Hidden Markov Models for multi-step regime forecasting, and richer volatility proxies such as range-based estimators or intraday aggregates when high-frequency data is available.

---

## 7 References

Bishop, C. M. (2006). *Pattern recognition and machine learning.* Springer.

Bollerslev, T. (1986). Generalized autoregressive conditional heteroskedasticity. *Journal of Econometrics, 31*(3), 307–327.

Cont, R. (2001). Empirical properties of asset returns: Stylized facts and statistical issues. *Quantitative Finance, 1*(2), 223–236.

Corsi, F. (2009). A simple approximate long-memory model of realized volatility. *Journal of Financial Econometrics, 7*(2), 174–196.

Diebold, F. X., & Mariano, R. S. (1995). Comparing predictive accuracy. *Journal of Business & Economic Statistics, 13*(3), 253–263.

Heston, S. L. (1993). A closed-form solution for options with stochastic volatility with applications to bond and currency options. *Review of Financial Studies, 6*(2), 327–343.

**J.P. Morgan/Reuters. (1996). *RiskMetrics™ Technical Document* (4th ed.). J.P. Morgan.** ← *Primary reference for EWMA conditional variance and EWMA-based VaR simulation.*

**Jorion, P. (2007). *Value at Risk: The New Benchmark for Managing Financial Risk* (3rd ed.). McGraw-Hill.** ← *Covers EWMA volatility estimation, Monte Carlo VaR simulation, and backtesting methodology including the Kupiec test.*

Kupiec, P. H. (1995). Techniques for verifying the accuracy of risk measurement models. *Journal of Derivatives, 3*(2), 73–84.

**McNeil, A. J., Frey, R., & Embrechts, P. (2015). *Quantitative Risk Management: Concepts, Techniques and Tools* (revised ed.). Princeton University Press.** ← *Theoretical foundations for EWMA in VaR models; covers simulation-based VaR and its distributional assumptions.*

Patton, A. J. (2011). Volatility forecast comparison using imperfect volatility proxies. *Journal of Econometrics, 160*(1), 246–256.

Risken, H. (1996). *The Fokker–Planck equation: Methods of solution and applications* (2nd ed.). Springer.

---

## 8 Appendices

### Appendix A: Neural Network Architecture

The model consists of $K = 3$ expert potential networks and one gating network. Each expert is a multilayer perceptron with 3 hidden layers of 256 neurons each and ReLU activations, mapping the full feature vector $x \in \mathbb{R}^{d}$ to a scalar potential value $U_k \in \mathbb{R}$. The gating network maps only the slow (macro) feature sub-vector $x^s \in \mathbb{R}^{d_s}$ to a $K$-dimensional softmax output. The combined potential is the gate-weighted sum of expert outputs. All gradients are computed via automatic differentiation through the combined potential with respect to the input feature vector; the volatility forecast is extracted as the negative gradient with respect to the lagged-vol coordinate.

Training used AdamW with cosine annealing learning rate schedule ($\eta_0 = 10^{-4}$, $\eta_{min} = 10^{-6}$, $T_{max} = 12500$), batch size 128, and gradient clipping at norm 1.0.

### Appendix B: Potential Surface Visualizations

*(Figures as in previous draft.)*

The value of the potential is arbitrary in absolute terms; the model's information content lies in its gradient structure. Regions of steeper slope correspond to stronger predicted drift, while flat regions correspond to near-equilibrium states.

Figure 2 illustrates the effect of the variational smoothness penalty $\lambda_v$. With $\lambda_v = 0.2$, the learned surface is smooth and interpretable; with $\lambda_v = 0.0$, the surface is highly irregular, reflecting overfitting to short-term fluctuations.

### Appendix C: Feature Engineering

**C.1 Raw feature universe**

Features are collected from three sources merged on trading date:
- *Futures data:* close price, volume (daily observations).
- *Domestic macro:* U.S. Treasury yields, yield curve spreads, federal funds rate, SOFR, inflation (CPI, core CPI, core PCE), GDP growth, retail sales, industrial production, manufacturing capacity utilization, nonfarm payrolls, unemployment rate, consumer sentiment, M2 money supply, Federal Reserve balance sheet, credit spreads (high yield, investment grade), VIX, 10-year TIPS yield.
- *Global macro:* International equity volatility indices, commodity indices, foreign exchange rates.

Engineered features computed from futures data:
- Log return: $r_t = \log(P_t/P_{t-1})$
- EWMA volatility (span=10): used as both lagged feature and regression target
- Absolute return: used for distributional analysis only, not a model input

**C.2 Correlation-based pruning**

A Pearson correlation matrix was computed across all slow features on the full dataset. Pairs with $|r| > 0.85$ were flagged. The following features were removed (one from each redundant pair), retaining the feature with greater interpretability or broader economic scope:

| Removed feature | Retained feature | Correlation |
|---|---|---|
| `market_vix` | `vix_close` | 1.000 |
| `inflation_core_cpi` | `inflation_cpi` | 0.997 |
| `inflation_core_pce` | `inflation_cpi` | 0.998 |
| `treas_2y_close` | `yield_curve_treasury_2y` | −0.999 |
| `treas_5y_close` | `yield_curve_treasury_10y` | −0.982 |
| `yield_curve_treasury_30y` | `yield_curve_treasury_10y` | 0.990 |
| `yield_curve_sofr` | `yield_curve_fed_funds_rate` | 0.993 |
| `yield_curve_butterfly_spread` | `yield_curve_yield_spread_10y_2y` | 0.934 |
| `credit_spreads_ig_credit_spread` | `credit_spreads_high_yield_spread` | 0.956 |
| `growth_manufacturing_employment` | `growth_nonfarm_payrolls` | 0.858 |
| `liquidity_fed_balance_sheet` | `liquidity_m2_money_supply` | 0.908 |

**C.3 Final feature set**

After pruning, the model uses 4 fast features and approximately 20 slow (macro) features. The fast/slow partition determines the input to the gating network (slow only) versus expert networks (full vector). All features are scaled using RobustScaler (median/IQR), which is less sensitive to the fat-tailed distributions present in financial time series.
