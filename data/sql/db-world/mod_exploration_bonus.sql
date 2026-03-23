--
-- Tabla para mapear Zonas con Logros de Exploración
--

CREATE TABLE IF NOT EXISTS `mod_exploration_bonus_zones` (
  `zone_id` INT UNSIGNED NOT NULL COMMENT 'ID de la zona (AreaTable.dbc)',
  `achievement_id` INT UNSIGNED NOT NULL COMMENT 'ID del logro de exploración (Achievement.dbc)',
  PRIMARY KEY (`zone_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

--
-- Mapeos iniciales (Reinos del Este y Kalimdor básicos)
--

REPLACE INTO `mod_exploration_bonus_zones` (`zone_id`, `achievement_id`) VALUES 
(12, 762),  -- Elwynn Forest
(40, 802),  -- Westfall
(10, 763),  -- Duskwood
(11, 847),  -- Wetlands
(1, 758),   -- Dun Morogh
(38, 764),  -- Loch Modan
(44, 767),  -- Redridge Mountains
(85, 768),  -- Tirisfal Glades
(130, 769), -- Silverpine Forest
(267, 770), -- Hillsbrad Foothills
(14, 765),  -- Durotar
(215, 766), -- Mulgore
(17, 761),  -- Barrens
(148, 843), -- Darkshore
(141, 842); -- Teldrassil
