import numpy as np
import torch

def simulate_one_step(model, fast_x, slow_x, vol_index, vol_mean, vol_std, dt, eps=1e-10, log_space=False):
    """
    Simulate one next-step volatility and return

    Returns:
        next_vol_raw
        next_return
        delta_mean_scaled
        delta_var_scaled
    """
    model.eval()
    fast_x_t = torch.tensor(fast_x, dtype=torch.float32).unsqueeze(0).requires_grad_(True)
    slow_x_t = None
    if slow_x is not None:
        slow_x_t = torch.tensor(slow_x, dtype=torch.float32).unsqueeze(0)

    # Keep a detached copy of the fast state for reading current lagged vol.
    x_t = fast_x_t.detach()

    out = model(fast_x_t, slow_x_t)
    # GatedPotentialSurrogate returns (potential, log_var, gate_weights, expert_potentials)
    # plain Surrogate returns (potential, log_var)
    potential, log_var = out[0], out[1]

    grads = torch.autograd.grad(
        potential,
        fast_x_t,
        grad_outputs=torch.ones_like(potential),
        create_graph=False
    )[0]

    # mean delta in scaled-vol units
    delta_mean_scaled = (-grads[:, vol_index] * dt).item()

    # variance of delta-vol error in scaled units
    delta_var_scaled = torch.exp(log_var).item()

    # current vol in the native feature space (raw or log depending on log_space)
    vol_feat_t = (x_t[0, vol_index].detach().item() * vol_std) + vol_mean

    # sample next vol feature value
    delta_noise_scaled = np.random.randn() * np.sqrt(max(2 * delta_var_scaled * dt, eps))
    next_vol_feat = vol_feat_t + (delta_mean_scaled + delta_noise_scaled) * vol_std

    if log_space:
        next_vol_raw = max(np.exp(next_vol_feat), eps)
    else:
        next_vol_raw = max(next_vol_feat, eps)

    # model predicts EWMA directly - no conversion needed
    next_sigma = max(next_vol_raw, eps)

    # simulate return
    next_return = np.random.randn() * next_sigma

    return next_vol_raw, next_return, delta_mean_scaled, delta_var_scaled


def monte_carlo_vol_return_paths(
    model,
    x0_scaled,
    n_steps,
    n_paths,
    vol_index,
    vol_mean,
    vol_std,
    dt,
    eps=1e-10,
    log_space=False,
    slow_x=None
):
    """
    Simulate volatility and return paths forward under fixed non-vol features.

    x0_scaled: 1D numpy array of scaled features at current time
    slow_x:    1D numpy array of scaled slow features (for gated models)
    """

    vol_paths = np.zeros((n_paths, n_steps + 1))
    return_paths = np.zeros((n_paths, n_steps))

    # initialize raw vol from x0
    if log_space:
        init_vol_raw = np.exp(x0_scaled[vol_index] * vol_std + vol_mean)
    else:
        init_vol_raw = x0_scaled[vol_index] * vol_std + vol_mean
    vol_paths[:, 0] = init_vol_raw

    for p in range(n_paths):
        x_curr = x0_scaled.copy()

        for t in range(n_steps):
            next_vol_raw, next_return, _, _ = simulate_one_step(
                model=model,
                fast_x=x_curr,
                slow_x=slow_x,
                vol_index=vol_index,
                vol_mean=vol_mean,
                vol_std=vol_std,
                dt=dt,
                eps=eps,
                log_space=log_space
            )

            vol_paths[p, t + 1] = next_vol_raw
            return_paths[p, t] = next_return

            # update lagged vol feature in scaled space
            if log_space:
                x_curr[vol_index] = (np.log(max(next_vol_raw, eps)) - vol_mean) / vol_std
            else:
                x_curr[vol_index] = (next_vol_raw - vol_mean) / vol_std

    return vol_paths, return_paths


def monte_carlo_nested_returns(
    model,
    x0_scaled,
    n_steps,
    n_vol_paths,
    n_return_paths_per_vol,
    vol_index,
    vol_mean,
    vol_std,
    dt,
    eps=1e-10,
    log_space=False,
    slow_x=None
):
    """
    First simulate volatility paths.
    Then, for each volatility path, simulate multiple return paths conditional on that path.
    """

    vol_paths, _ = monte_carlo_vol_return_paths(
        model=model,
        x0_scaled=x0_scaled,
        n_steps=n_steps,
        n_paths=n_vol_paths,
        vol_index=vol_index,
        vol_mean=vol_mean,
        vol_std=vol_std,
        dt=dt,
        eps=eps,
        log_space=log_space,
        slow_x=slow_x
    )

    nested_returns = np.zeros((n_vol_paths, n_return_paths_per_vol, n_steps))

    for i in range(n_vol_paths):
        for j in range(n_return_paths_per_vol):
            for t in range(n_steps):
                vol_raw = max(vol_paths[i, t + 1], eps)
                # model predicts EWMA σ directly – no conversion needed
                nested_returns[i, j, t] = np.random.randn() * vol_raw

    return vol_paths, nested_returns