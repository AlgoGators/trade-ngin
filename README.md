flowchart TB
    subgraph BacktestConfig
        BC[BacktestConfig]
        SC[StrategyConfig]
        PBC[PortfolioBacktestConfig]
        BC --> SC
        BC --> PBC
    end
    
    subgraph StrategyConfigs
        TFC[TrendFollowingConfig]
        SC --> TFC
    end
    
    subgraph PortfolioConfig
        PC[PortfolioConfig]
        RC[RiskConfig]
        OC[OptimizationConfig]
        PC --> RC
        PC --> OC
    end
    
    PBC --> PC
    
    style BC fill:#f9f,stroke:#333,stroke-width:2px
    style PC fill:#bbf,stroke:#333,stroke-width:2px
    style SC fill:#bfb,stroke:#333,stroke-width:2px
    style TFC fill:#bfb,stroke:#333,stroke-width:2px