import torch
import torch.nn as nn
from torch.utils.data import TensorDataset, DataLoader
from sklearn.model_selection import train_test_split
import numpy as np
import pandas as pd


class Gate(nn.Module):
    def __init__(self, input_size, hidden_size, K):
        super().__init__()
        self.model = nn.Sequential(
            nn.Linear(input_size, hidden_size),
            nn.ReLU(),
            nn.Linear(hidden_size, K),
            nn.Softmax(dim=1)
        )

    def forward(self, slow_features):
        gate_outputs = self.model(slow_features)
        return gate_outputs
    

class Surrogate(nn.Module):
    def __init__(self, input_size, hidden_layers, neurons_per_layer, activation_function):
        super().__init__()

        layers = [nn.Linear(input_size, neurons_per_layer), activation_function()]
        for _ in range(hidden_layers - 1):
            layers += [nn.Linear(neurons_per_layer, neurons_per_layer), activation_function()]

        self.backbone = nn.Sequential(*layers)
        self.potential_head = nn.Linear(neurons_per_layer, 1)
        self.logvar_head = nn.Linear(neurons_per_layer, 1)

    def forward(self, fast_features, slow_features=None):
        if slow_features is not None:
            x = torch.cat((fast_features, slow_features), dim=1)
        else:
            x = fast_features

        h = self.backbone(x)
        potential = self.potential_head(h).squeeze(-1)   
        log_var = self.logvar_head(h).squeeze(-1)        
        return potential, log_var
    

class PotentialGradSurrogate(nn.Module):
    def __init__(self, fast_dim, slow_dim, K, hidden_layers, neurons_per_layer, activation_function):
        super().__init__()
        self.gate = Gate(slow_dim, neurons_per_layer, K)
        self.surrogate = Surrogate(fast_dim + slow_dim, K, hidden_layers, neurons_per_layer, activation_function)

    def forward(self, fast_features, slow_features):
        gate_outputs = self.gate(slow_features)
        surrogate_outputs = self.surrogate(fast_features, slow_features)
        gated_surrogate_output = torch.sum(gate_outputs * surrogate_outputs, dim=1)
        gradient = torch.autograd.grad(gated_surrogate_output.sum(), fast_features, create_graph=True)[0]
        return -gradient


class GatedPotentialSurrogate(nn.Module):
    """
    Mixture-of-experts potential model.
    K expert networks each learn a separate potential surface.
    A gate network (driven only by slow macro features) produces mixture weights.
    Entropy regularization ensures the gate distributes weight across experts.
    """
    def __init__(self, input_dim, slow_dim, K, hidden_layers, neurons_per_layer, activation_function):
        super().__init__()
        self.K = K

        # K potential networks - each sees ALL features
        self.experts = nn.ModuleList([
            self._build_expert(input_dim, hidden_layers, neurons_per_layer, activation_function)
            for _ in range(K)
        ])

        # Gate network - sees only slow (macro) features
        self.gate = nn.Sequential(
            nn.Linear(slow_dim, neurons_per_layer),
            nn.ReLU(),
            nn.Linear(neurons_per_layer, neurons_per_layer // 2),
            nn.ReLU(),
            nn.Linear(neurons_per_layer // 2, K),
            nn.Softmax(dim=1)
        )

    @staticmethod
    def _build_expert(input_dim, hidden_layers, neurons_per_layer, act):
        layers = [nn.Linear(input_dim, neurons_per_layer), act()]
        for _ in range(hidden_layers - 1):
            layers += [nn.Linear(neurons_per_layer, neurons_per_layer), act()]
        base = nn.Sequential(*layers)
        return nn.ModuleDict({
            'backbone': base,
            'potential_head': nn.Linear(neurons_per_layer, 1),
            'logvar_head': nn.Linear(neurons_per_layer, 1),
        })

    def forward(self, x, slow_features):
        """
        Returns:
            combined_potential: (B,) gated sum of expert potentials
            combined_logvar:    (B,) gated sum of expert log-variances
            gate_weights:       (B, K) soft assignment probabilities
            expert_potentials:  (B, K) each expert's raw potential
        """
        gate_weights = self.gate(slow_features)

        potentials = []
        logvars = []
        for expert in self.experts:
            h = expert['backbone'](x)
            potentials.append(expert['potential_head'](h).squeeze(-1))
            logvars.append(expert['logvar_head'](h).squeeze(-1))

        expert_potentials = torch.stack(potentials, dim=1)
        expert_logvars = torch.stack(logvars, dim=1)

        combined_potential = (gate_weights * expert_potentials).sum(dim=1)
        combined_logvar = (gate_weights * expert_logvars).sum(dim=1)

        return combined_potential, combined_logvar, gate_weights, expert_potentials

    """Evaluate a single expert (for plotting individual surfaces)"""
    def forward_expert(self, x, expert_idx):
        expert = self.experts[expert_idx]
        h = expert['backbone'](x)
        potential = expert['potential_head'](h).squeeze(-1)
        logvar = expert['logvar_head'](h).squeeze(-1)
        return potential, logvar