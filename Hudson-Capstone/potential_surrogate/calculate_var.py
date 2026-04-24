import numpy as np

from monte_carlo import monte_carlo_nested_returns

def compute_var_from_nested_returns(
    nested_returns,
    alpha=0.05,
    horizon=1,
    return_type="simple",
    position_value=1.0
):
    """
    Compute VaR from nested simulated returns.

    Returns
    dict with:
        var_return : VaR in return units
        var_dollar : VaR in dollar units
        quantile_return : raw lower-tail return quantile
        aggregated_returns : flattened simulated horizon returns
    """
    if horizon < 1 or horizon > nested_returns.shape[2]:
        raise ValueError("horizon must be between 1 and n_steps")

    sim_slice = nested_returns[:, :, :horizon]

    if return_type == "simple":
        # compound simple returns
        agg_returns = np.prod(1.0 + sim_slice, axis=2) - 1.0
    elif return_type == "log":
        # add log returns
        agg_returns = np.sum(sim_slice, axis=2)
    else:
        raise ValueError("return_type must be 'simple' or 'log'")

    agg_returns_flat = agg_returns.reshape(-1)

    q_alpha = np.quantile(agg_returns_flat, alpha)
    var_return = -q_alpha
    var_dollar = position_value * var_return

    return {
        "var_return": var_return,
        "var_dollar": var_dollar,
        "quantile_return": q_alpha,
        "aggregated_returns": agg_returns_flat
    }

def one_step_model_var_from_nested_mc(
    model,
    x0_scaled,
    n_vol_paths,
    n_return_paths_per_vol,
    n_fast_features,
    vol_index,
    vol_mean,
    vol_std,
    dt,
    alpha=0.05,
    eps=1e-10,
    log_space=False,
    slow_x=None
):
    """
    Compute 1-step VaR forecast from nested Monte Carlo
    Returns the alpha-quantile of next-step simulated returns
    """

    _, nested_returns = monte_carlo_nested_returns(
        model=model,
        x0_scaled=x0_scaled,
        n_steps=1,
        n_vol_paths=n_vol_paths,
        n_return_paths_per_vol=n_return_paths_per_vol,
        vol_index=vol_index,
        vol_mean=vol_mean,
        vol_std=vol_std,
        dt=dt,
        eps=eps,
        log_space=log_space,
        slow_x=slow_x
    )

    one_step_returns = nested_returns[:, :, 0].reshape(-1)
    return np.quantile(one_step_returns, alpha)