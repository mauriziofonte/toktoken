package service

import scala.util.{Try, Success, Failure}

class Processor {
  private var processedCount: Int = 0

  def execute(input: String): String = {
    val result = Try {
      val tokens = input.split("\\s+").toList
      val transformed = transform(tokens)
      processedCount += 1
      transformed.mkString(" ")
    }

    result match {
      case Success(output) => output
      case Failure(ex)     => s"ERROR: ${ex.getMessage}"
    }
  }

  def validate(input: String): Option[String] = {
    if (input == null || input.trim.isEmpty)
      Some("Input must not be empty")
    else if (input.length > 10000)
      Some(s"Input too large: ${input.length} chars (max 10000)")
    else if (input.exists(c => c < 32 && c != '\n' && c != '\t'))
      Some("Input contains invalid control characters")
    else
      None
  }

  def transform(tokens: List[String]): List[String] = {
    tokens
      .filter(_.nonEmpty)
      .map(_.trim.toLowerCase)
      .distinct
  }

  def stats: Map[String, Int] = Map(
    "processed" -> processedCount
  )
}
