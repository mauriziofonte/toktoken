module Lib.Parser (parseExpression, tokenize, evaluate, Expr(..)) where

data Expr
    = Num Double
    | Add Expr Expr
    | Mul Expr Expr
    | Neg Expr
    deriving (Show, Eq)

data Token = TNum Double | TPlus | TStar | TMinus | TLParen | TRParen
    deriving (Show)

tokenize :: String -> Either String [Token]
tokenize [] = Right []
tokenize (c:cs)
    | c == ' '  = tokenize cs
    | c == '+'  = fmap (TPlus :) (tokenize cs)
    | c == '*'  = fmap (TStar :) (tokenize cs)
    | c == '-'  = fmap (TMinus :) (tokenize cs)
    | c == '('  = fmap (TLParen :) (tokenize cs)
    | c == ')'  = fmap (TRParen :) (tokenize cs)
    | isDigitChar c = let (num, rest) = span isNumChar (c:cs)
                      in case reads num :: [(Double, String)] of
                           [(n, "")] -> fmap (TNum n :) (tokenize rest)
                           _         -> Left $ "Invalid number: " ++ num
    | otherwise = Left $ "Unexpected character: " ++ [c]
  where
    isDigitChar x = x >= '0' && x <= '9'
    isNumChar x   = isDigitChar x || x == '.'

parseExpression :: String -> Either String Expr
parseExpression input = do
    tokens <- tokenize input
    case tokens of
        [TNum n]           -> Right (Num n)
        [TNum a, TPlus, TNum b]  -> Right (Add (Num a) (Num b))
        [TNum a, TStar, TNum b]  -> Right (Mul (Num a) (Num b))
        [TMinus, TNum n]         -> Right (Neg (Num n))
        []                       -> Left "Empty expression"
        _                        -> Left "Complex expressions not yet supported"

evaluate :: Expr -> Either String Double
evaluate (Num n)   = Right n
evaluate (Add a b) = (+) <$> evaluate a <*> evaluate b
evaluate (Mul a b) = (*) <$> evaluate a <*> evaluate b
evaluate (Neg e)   = negate <$> evaluate e
