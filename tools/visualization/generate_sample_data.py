#!/usr/bin/env python3
"""
Sample Backtest Data Generator

This script generates sample CSV files that mimic the output of the Trade-Ngin backtest engine.
Use this to test the visualization tools without having to run a full backtest.

Usage:
    python3 generate_sample_data.py --output-dir <directory>

Generated files:
    - results.csv: Main backtest metrics
    - equity_curve.csv: Daily equity values
    - trade_executions.csv: Trade execution details
    - symbol_pnl.csv: P&L breakdown by symbol
    - final_positions.csv: Final positions at end of backtest
"""

import pandas as pd
import numpy as np
import os
import argparse
import datetime
import uuid
from pathlib import Path

def generate_sample_data(output_dir):
    """Generate sample backtest data files"""
    os.makedirs(output_dir, exist_ok=True)
    run_id = str(uuid.uuid4())
    
    # Generate date range for 2 years of daily data
    end_date = datetime.datetime.now().date()
    start_date = end_date - datetime.timedelta(days=2*365)
    dates = pd.date_range(start=start_date, end=end_date, freq='D')
    
    # Sample instrument symbols
    symbols = ["ES.v.0", "NQ.v.0", "CL.v.0", "GC.v.0", "ZB.v.0", 
               "ZN.v.0", "ZF.v.0", "ZC.v.0", "ZW.v.0", "ZS.v.0",
               "6E.v.0", "6J.v.0", "6B.v.0", "6C.v.0", "6A.v.0"]
    
    # 1. Generate equity curve data
    print(f"Generating equity curve data...")
    initial_equity = 1000000  # $1M
    
    # Create a random walk with drift for equity
    np.random.seed(42)  # For reproducibility
    returns = np.random.normal(0.0003, 0.01, len(dates))  # Mean positive return
    equity_values = initial_equity * np.cumprod(1 + returns)
    
    equity_df = pd.DataFrame({
        'timestamp': dates,
        'equity': equity_values
    })
    
    equity_df.to_csv(os.path.join(output_dir, 'equity_curve.csv'), index=False)
    
    # 2. Generate trade executions
    print(f"Generating trade execution data...")
    n_trades = 500
    trade_dates = np.random.choice(dates, n_trades)
    trade_dates.sort()
    
    execution_ids = [str(uuid.uuid4()) for _ in range(n_trades)]
    trade_symbols = np.random.choice(symbols, n_trades)
    sides = np.random.choice(['BUY', 'SELL'], n_trades)
    quantities = np.random.randint(1, 10, n_trades) * 10  # 10, 20, ..., 90 contracts
    prices = np.random.uniform(50, 5000, n_trades)
    commissions = prices * quantities * 0.0005  # 0.05% commission
    
    trades_df = pd.DataFrame({
        'run_id': [run_id] * n_trades,
        'execution_id': execution_ids,
        'timestamp': trade_dates,
        'symbol': trade_symbols,
        'side': sides,
        'quantity': quantities * np.where(sides == 'BUY', 1, -1),  # Negative for sells
        'price': prices,
        'commission': commissions
    })
    
    trades_df.to_csv(os.path.join(output_dir, 'trade_executions.csv'), index=False)
    
    # 3. Generate symbol P&L data
    print(f"Generating symbol P&L data...")
    symbol_pnls = []
    
    for symbol in symbols:
        # Generate random P&L with some symbols profitable, some not
        pnl = np.random.normal(5000, 50000)
        symbol_pnls.append({
            'run_id': run_id,
            'symbol': symbol,
            'pnl': pnl
        })
    
    symbol_pnl_df = pd.DataFrame(symbol_pnls)
    symbol_pnl_df.to_csv(os.path.join(output_dir, 'symbol_pnl.csv'), index=False)
    
    # 4. Generate final positions
    print(f"Generating final positions data...")
    positions = []
    
    for symbol in symbols:
        if np.random.random() > 0.3:  # 70% chance of having a position
            quantity = np.random.randint(-5, 5) * 10
            if quantity != 0:
                avg_price = np.random.uniform(50, 5000)
                unrealized_pnl = np.random.normal(1000, 5000)
                realized_pnl = np.random.normal(2000, 10000)
                
                positions.append({
                    'run_id': run_id,
                    'symbol': symbol,
                    'quantity': quantity,
                    'average_price': avg_price,
                    'unrealized_pnl': unrealized_pnl,
                    'realized_pnl': realized_pnl
                })
    
    positions_df = pd.DataFrame(positions)
    positions_df.to_csv(os.path.join(output_dir, 'final_positions.csv'), index=False)
    
    # 5. Generate main results
    print(f"Generating main results data...")
    
    # Calculate actual final performance metrics
    final_equity = equity_df['equity'].iloc[-1]
    total_return = (final_equity / initial_equity) - 1
    
    # Calculate drawdowns for max drawdown
    peaks = equity_df['equity'].cummax()
    drawdowns = (equity_df['equity'] - peaks) / peaks
    max_drawdown = drawdowns.min() * -1
    
    # Calculate daily returns for volatility
    daily_returns = equity_df['equity'].pct_change().dropna()
    volatility = daily_returns.std() * np.sqrt(252)  # Annualized
    
    # Count winning trades (simplified)
    win_rate = 0.55  # 55% win rate
    
    # Calculate other metrics
    sharpe_ratio = (daily_returns.mean() * 252) / volatility if volatility > 0 else 0
    sortino_ratio = sharpe_ratio * 1.2  # Simplified
    calmar_ratio = (total_return / max_drawdown) if max_drawdown > 0 else 0
    
    results = [{
        'run_id': run_id,
        'start_date': start_date.strftime('%Y-%m-%d'),
        'end_date': end_date.strftime('%Y-%m-%d'),
        'total_return': total_return,
        'sharpe_ratio': sharpe_ratio,
        'sortino_ratio': sortino_ratio,
        'max_drawdown': max_drawdown,
        'calmar_ratio': calmar_ratio,
        'volatility': volatility,
        'total_trades': n_trades,
        'win_rate': win_rate,
        'profit_factor': 1.2,
        'avg_win': 2000,
        'avg_loss': -1500,
        'max_win': 15000,
        'max_loss': -10000,
        'avg_holding_period': 5.3,
        'var_95': -0.02,
        'cvar_95': -0.03,
        'beta': 0.8,
        'correlation': 0.65,
        'downside_volatility': 0.09,
        'config': '{"initial_capital":1000000,"symbols":["ES.v.0","NQ.v.0","CL.v.0","GC.v.0","ZB.v.0","ZN.v.0","ZF.v.0","ZC.v.0","ZW.v.0","ZS.v.0","6E.v.0","6J.v.0","6B.v.0","6C.v.0","6A.v.0"]}'
    }]
    
    results_df = pd.DataFrame(results)
    results_df.to_csv(os.path.join(output_dir, 'results.csv'), index=False)
    
    print(f"Sample data successfully generated in: {output_dir}")
    return output_dir

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Generate sample backtest data')
    parser.add_argument('--output-dir', default='apps/backtest/results/BT_sample', 
                        help='Directory to save sample data files')
    args = parser.parse_args()
    
    generate_sample_data(args.output_dir) 