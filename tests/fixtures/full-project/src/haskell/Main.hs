module Main where

import Lib.Parser (parseExpression, evaluate)
import System.IO (hFlush, stdout, hSetBuffering, BufferMode(..))

main :: IO ()
main = do
    hSetBuffering stdout LineBuffering
    putStrLn "Expression Evaluator v1.0"
    putStrLn "Enter expressions (empty line to quit):"
    repl

repl :: IO ()
repl = do
    putStr "> "
    hFlush stdout
    input <- getLine
    case input of
        "" -> putStrLn "Goodbye."
        _  -> do
            processInput input
            repl

processInput :: String -> IO ()
processInput raw = do
    let trimmed = dropWhile (== ' ') raw
    case parseExpression trimmed of
        Left err   -> putStrLn $ "Parse error: " ++ err
        Right expr -> case evaluate expr of
            Left err  -> putStrLn $ "Eval error: " ++ err
            Right val -> putStrLn $ "= " ++ show val
